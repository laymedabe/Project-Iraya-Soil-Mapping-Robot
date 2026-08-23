"""
GPS waypoint navigator for DUTA.

Runs a background thread that steers the robot through a sequence of GPS
waypoints using course-over-ground from the NEO-M8 GPS module.  The Pi
queries the Mega for GPS data, computes the bearing to the next waypoint,
and issues DRIVE commands to correct the heading.

Steering strategy (course-over-ground, no compass):
  - COG is only valid while the robot is moving forward.
  - If bearing error is within tolerance → DRIVE FWD (on course).
  - If bearing error exceeds tolerance → DRIVE LEFT or RIGHT to correct.
  - When within ARRIVAL_RADIUS of a waypoint → STOP + SAMPLE.
  - After sample completes → advance to next waypoint.
"""

import math
import time
import logging
import threading
from app.config import Config
from app.gps_reader import gps_reader
from app.npk_reader import npk_reader

logger = logging.getLogger("iraya.navigator")

# ------------------------------------------------------------------ config

NAV_LOOP_HZ = 5                  # Navigation corrections per second
NAV_LOOP_INTERVAL = 1.0 / NAV_LOOP_HZ
ARRIVAL_RADIUS_M = 2.0           # Close enough to sample (meters)
HEADING_TOLERANCE_DEG = 20.0     # Acceptable bearing error for straight driving
NAV_SPEED = 180                  # PWM speed during navigation (0-255)
SAMPLE_POLL_INTERVAL = 0.5       # Seconds between actuator status checks
INITIAL_DRIVE_TIME = 3.0         # Drive forward this long before trusting COG
MIN_SPEED_FOR_COG = 0.3          # m/s — below this, COG is unreliable

# ------------------------------------------------------------------ math

def haversine_distance(lat1, lon1, lat2, lon2):
    """Distance in meters between two GPS coordinates (Haversine formula)."""
    R = 6_371_000  # Earth radius in meters
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2) ** 2
    return R * 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))


def compute_bearing(lat1, lon1, lat2, lon2):
    """Initial bearing in degrees (0-360) from point 1 to point 2."""
    phi1, phi2 = math.radians(lat1), math.radians(lat2)
    dlam = math.radians(lon2 - lon1)
    x = math.sin(dlam) * math.cos(phi2)
    y = math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(dlam)
    bearing = math.degrees(math.atan2(x, y))
    return (bearing + 360) % 360


def normalize_angle(angle):
    """Normalize an angle to the range -180..+180."""
    while angle > 180:
        angle -= 360
    while angle < -180:
        angle += 360
    return angle


# ------------------------------------------------------------------ navigator

class Navigator:
    """GPS waypoint navigator that runs in a background thread."""

    def __init__(self, mega_link):
        self._mega = mega_link
        self._thread = None
        self._stop_event = threading.Event()
        self._waypoints = []
        self._session_id = None
        self._current_wp_index = 0
        self._on_sample_complete = None  # callback(session_id, waypoint)
        self._on_navigation_done = None  # callback(session_id)

    @property
    def is_running(self):
        return self._thread is not None and self._thread.is_alive()

    @property
    def current_waypoint_index(self):
        return self._current_wp_index

    def start(self, session_id, waypoints, on_sample_complete=None, on_navigation_done=None):
        """Begin navigating through the given waypoints.

        Args:
            session_id: The active session ID for logging.
            waypoints: List of dicts with 'lat', 'lon', and optionally 'id'.
            on_sample_complete: Callback(session_id, waypoint_dict, gps_reading)
                                called after each successful sample.
            on_navigation_done: Callback(session_id) called when all waypoints
                                are visited or navigation is stopped.
        """
        if self.is_running:
            logger.warning("Navigator already running — stop first.")
            return False

        self._session_id = session_id
        self._waypoints = list(waypoints)
        self._current_wp_index = 0
        self._on_sample_complete = on_sample_complete
        self._on_navigation_done = on_navigation_done
        self._stop_event.clear()

        self._thread = threading.Thread(target=self._nav_loop, daemon=True,
                                         name="navigator")
        self._thread.start()
        logger.info(f"Navigator started — {len(waypoints)} waypoints for session {session_id}")
        return True

    def stop(self):
        """Cancel navigation and stop the robot."""
        if not self.is_running:
            return
        self._stop_event.set()
        try:
            self._mega.send_command("STOP")
        except Exception as exc:
            logger.error(f"Failed to send STOP during nav cancel: {exc}")
        # Don't join — the thread will exit on its own
        logger.info("Navigator stop requested")

    # ----------------------------------------------------------- main loop

    def _nav_loop(self):
        """Main navigation loop — runs in a background thread."""
        logger.info("Navigation loop started")

        try:
            while self._current_wp_index < len(self._waypoints):
                if self._stop_event.is_set():
                    break

                wp = self._waypoints[self._current_wp_index]
                target_lat = float(wp["lat"])
                target_lon = float(wp["lon"])

                logger.info(
                    f"Navigating to waypoint {self._current_wp_index + 1}/"
                    f"{len(self._waypoints)}: ({target_lat:.6f}, {target_lon:.6f})"
                )
                self._mega.send_command(f"GOTO {target_lat} {target_lon}")

                # --- Phase 1: Drive toward the waypoint ---
                arrived = self._drive_to_waypoint(target_lat, target_lon)
                if not arrived:
                    break  # stop_event was set

                # --- Phase 2: Stop and take a soil sample ---
                logger.info(f"Arrived at waypoint {self._current_wp_index + 1} — sampling")
                self._mega.send_command("STOP")
                time.sleep(0.3)  # Brief settle time

                sample_data = self._take_sample()
                if not sample_data:
                    if self._stop_event.is_set():
                        break
                    logger.warning("Sample failed or timed out — moving to next waypoint")
                else:
                    # Notify callback
                    if self._on_sample_complete:
                        try:
                            self._on_sample_complete(self._session_id, wp, sample_data)
                        except Exception as exc:
                            logger.error(f"Sample complete callback failed: {exc}")

                self._current_wp_index += 1

            # --- Navigation complete ---
            if not self._stop_event.is_set():
                logger.info(f"All {len(self._waypoints)} waypoints visited!")
                self._mega.send_command("STOP")

        except Exception as exc:
            logger.error(f"Navigation loop crashed: {exc}", exc_info=True)
            try:
                self._mega.send_command("STOP")
            except Exception:
                pass

        finally:
            if self._on_navigation_done:
                try:
                    self._on_navigation_done(self._session_id)
                except Exception as exc:
                    logger.error(f"Navigation done callback failed: {exc}")

            logger.info("Navigation loop ended")

    # ----------------------------------------------------------- drive phase

    def _drive_to_waypoint(self, target_lat, target_lon):
        """Steer toward the target using GPS COG. Returns True if arrived."""
        # Start moving forward to get initial COG readings
        self._mega.send_command(f"DRIVE FWD {NAV_SPEED}")
        initial_drive_start = time.time()
        cog_trusted = False

        while not self._stop_event.is_set():
            loop_start = time.time()

            # Query GPS
            gps = gps_reader.get_position()
            if not gps["fix"]:
                # No GPS fix — keep driving forward, hope it comes back
                self._mega.send_command(f"DRIVE FWD {NAV_SPEED}")
                self._sleep_until(loop_start + NAV_LOOP_INTERVAL)
                continue

            # Check distance to waypoint
            distance = haversine_distance(gps["lat"], gps["lon"],
                                          target_lat, target_lon)

            if distance < ARRIVAL_RADIUS_M:
                return True  # Arrived!

            # Determine if COG is trustworthy
            time_driving = time.time() - initial_drive_start
            if not cog_trusted:
                if time_driving > INITIAL_DRIVE_TIME and gps["spd"] > MIN_SPEED_FOR_COG:
                    cog_trusted = True
                else:
                    # Still building momentum — just go straight
                    self._mega.send_command(f"DRIVE FWD {NAV_SPEED}")
                    self._sleep_until(loop_start + NAV_LOOP_INTERVAL)
                    continue

            # Compute desired bearing and error
            desired_bearing = compute_bearing(gps["lat"], gps["lon"],
                                              target_lat, target_lon)
            bearing_error = normalize_angle(desired_bearing - gps["cog"])

            # Steering decision
            if abs(bearing_error) < HEADING_TOLERANCE_DEG:
                # On course — drive straight
                self._mega.send_command(f"DRIVE FWD {NAV_SPEED}")
            elif bearing_error > 0:
                # Need to turn right
                self._mega.send_command(f"DRIVE RIGHT {NAV_SPEED}")
            else:
                # Need to turn left
                self._mega.send_command(f"DRIVE LEFT {NAV_SPEED}")

            logger.debug(
                f"NAV: dist={distance:.1f}m brg={desired_bearing:.0f}° "
                f"cog={gps['cog']:.0f}° err={bearing_error:.0f}° "
                f"spd={gps['spd']:.2f}m/s"
            )

            self._sleep_until(loop_start + NAV_LOOP_INTERVAL)

        return False  # Cancelled

    # ----------------------------------------------------------- sample phase

    def _take_sample(self):
        """Send SAMPLE command and wait for actuator to return to IDLE. Returns sample data or None."""
        try:
            self._mega.send_command("SAMPLE")
        except Exception as exc:
            logger.error(f"Failed to send SAMPLE: {exc}")
            return None

        # Wait for the actuator cycle to complete (extend → hold → retract → IDLE)
        # Typical cycle: 3s extend + 5s hold + 3s retract = ~11s
        timeout = 30.0
        start = time.time()
        
        sample_data = None
        has_sampled = False

        while time.time() - start < timeout:
            if self._stop_event.is_set():
                return None
            step = self._mega.state.get("step", "")
            
            if step == "READING" and not has_sampled:
                # We are holding the probe in the soil, take the reading!
                time.sleep(1.5) # Wait a bit for sensor to stabilize
                gps = gps_reader.get_position()
                npk = npk_reader.read_npk()
                
                # Format into a data dict
                sample_data = {
                    "lat": gps["lat"],
                    "lon": gps["lon"],
                    "altitude": gps["alt"],
                    "satellites": gps["sats"],
                    "hdop": gps["hdop"],
                    "no_gps_fix": 1 if not gps["fix"] else 0
                }
                
                if npk:
                    sample_data.update({
                        "nitrogen": npk["nitrogen"],
                        "phosphorus": npk["phosphorus"],
                        "potassium": npk["potassium"]
                    })
                else:
                    sample_data["npk_error"] = 1
                    
                has_sampled = True

            if step == "IDLE" and (time.time() - start) > 2.0:
                # IDLE and we've waited at least 2s (to avoid catching
                # the pre-LOWERING IDLE state)
                return sample_data
            time.sleep(SAMPLE_POLL_INTERVAL)

        logger.warning("Sample timed out after 30s")
        return sample_data

    # ----------------------------------------------------------- helpers

    def _sleep_until(self, target_time):
        """Sleep until target_time, but wake early if stop_event is set."""
        remaining = target_time - time.time()
        if remaining > 0:
            self._stop_event.wait(timeout=remaining)
