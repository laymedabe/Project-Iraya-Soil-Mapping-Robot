"""
UART bridge between the Flask app and the Arduino Mega.

Design notes
------------
- A single background thread owns the serial port and continuously reads
  lines, decoding STATUS/DATA/ACK/FAULT messages and pushing them onto a
  thread-safe queue. Flask routes/SocketIO handlers only ever call
  `send_command()` and `get_latest_state()` — they never touch the serial
  port directly. This avoids concurrent read/write races.
- `SERIAL_SIMULATE=true` runs a fake Mega in-process (used for developing
  the web app on a laptop with no hardware attached), so the rest of the
  codebase does not need to branch on hardware availability.

Data line format (from the real Arduino firmware):
  DATA POINT=1 LAT=14.9985 LON=121.0001 ALT=45.20 SAT=8 HDOP=1.05 N=42.3 P=15.1
"""

import threading
import queue
import time
import random
import logging
from app.config import Config

logger = logging.getLogger("iraya.serial")

try:
    import serial  # pyserial
except ImportError:  # allows import to succeed even if pyserial missing in simulate mode
    serial = None


class MegaLink:
    """Owns the serial connection to the Arduino Mega."""

    def __init__(self):
        self._ser = None
        self._reader_thread = None
        self._running = False
        self._event_queue = queue.Queue()
        self._lock = threading.Lock()

        # Latest known state, updated by the reader thread, read by routes.
        self.state = {
            "connected": False,
            "step": "IDLE",
            "last_fault": None,
        }

    # ------------------------------------------------------------ lifecycle

    def start(self):
        if Config.SERIAL_SIMULATE:
            logger.warning("SERIAL_SIMULATE=true — running fake Mega, no hardware attached.")
            self._running = True
            self._reader_thread = threading.Thread(target=self._simulate_loop, daemon=True)
            self._reader_thread.start()
            self.state["connected"] = True
            return

        if serial is None:
            raise RuntimeError("pyserial not installed — run `pip install pyserial`")

        self._ser = serial.Serial(
            Config.SERIAL_PORT, Config.SERIAL_BAUD, timeout=Config.SERIAL_TIMEOUT
        )
        self._running = True
        self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
        self._reader_thread.start()
        self.state["connected"] = True
        logger.info(f"Connected to Mega on {Config.SERIAL_PORT} @ {Config.SERIAL_BAUD}")

    def stop(self):
        self._running = False
        if self._ser:
            self._ser.close()

    # ------------------------------------------------------------ writing

    def send_command(self, command: str):
        """Send a single line command to the Mega. Thread-safe."""
        line = command.strip() + "\n"
        with self._lock:
            if Config.SERIAL_SIMULATE:
                logger.info(f"[SIM->MEGA] {command}")
                self._handle_simulated_command(command)
                return
            if self._ser is None:
                raise RuntimeError("Serial link not started")
            self._ser.write(line.encode("utf-8"))

    # ------------------------------------------------------------ reading

    def _read_loop(self):
        while self._running:
            try:
                raw = self._ser.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="ignore").strip()
                if line:
                    self._handle_line(line)
            except Exception as exc:
                logger.error(f"Serial read error: {exc}")
                time.sleep(0.5)

    def _handle_line(self, line: str):
        logger.debug(f"[MEGA->PI] {line}")
        
        # We need a buffer to accumulate the multi-line sensor readings
        if not hasattr(self, "_sensor_buffer"):
            self._sensor_buffer = {}

        if line.startswith("STATUS"):
            step = line.split(" ", 1)[1] if " " in line else line
            self.state["step"] = step
        elif line.startswith("FAULT"):
            self.state["last_fault"] = line
            self._event_queue.put({"type": "fault", "payload": line})
        
        # Parse human-readable 7-in-1 sensor output
        elif "Moisture" in line and ":" in line:
            try: self._sensor_buffer["moisture"] = float(line.split(":")[1].replace("%", "").strip())
            except ValueError: pass
        elif "Temperature" in line and ":" in line:
            try: self._sensor_buffer["temperature"] = float(line.split(":")[1].replace("deg C", "").strip())
            except ValueError: pass
        elif "EC" in line and ":" in line:
            try: self._sensor_buffer["ec"] = float(line.split(":")[1].replace("us/cm", "").strip()) / 1000.0 # Convert to dS/m
            except ValueError: pass
        elif "pH Level" in line and ":" in line:
            try: self._sensor_buffer["ph"] = float(line.split(":")[1].strip())
            except ValueError: pass
        elif "Nitrogen (N)" in line and ":" in line:
            try: self._sensor_buffer["nitrogen"] = float(line.split(":")[1].replace("mg/kg", "").strip())
            except ValueError: pass
        elif "Phosphorus" in line and ":" in line:
            try: self._sensor_buffer["phosphorus"] = float(line.split(":")[1].replace("mg/kg", "").strip())
            except ValueError: pass
        elif "Potassium" in line and ":" in line:
            try: 
                self._sensor_buffer["potassium"] = float(line.split(":")[1].replace("mg/kg", "").strip())
                
                # Potassium is the last line in the block. We have all data now!
                npk_data = {
                    "nitrogen": self._sensor_buffer.get("nitrogen", 0),
                    "phosphorus": self._sensor_buffer.get("phosphorus", 0),
                    "potassium": self._sensor_buffer.get("potassium", 0),
                    "moisture": self._sensor_buffer.get("moisture", 0),
                    "temperature": self._sensor_buffer.get("temperature", 0),
                    "ec": self._sensor_buffer.get("ec", 0),
                    "ph": self._sensor_buffer.get("ph", 0)
                }
                self.state["latest_npk"] = npk_data
                
                # Trigger the database capture right now
                threading.Thread(target=self._capture_sample, args=(npk_data,), daemon=True).start()
                
                # Clear buffer for next time
                self._sensor_buffer = {}
            except ValueError: pass
            
        elif line.startswith("ACK"):
            pass  # command acknowledged, nothing to do beyond logging

    def _capture_sample(self, npk):
        """Called automatically when Mega sends DATA NPK"""
        try:
            from app.gps_reader import gps_reader
            from app import models
            
            gps = gps_reader.get_position()
            
            session_id = models.get_or_create_default_session()
            reading_id = models.insert_reading(
                session_id, None, gps["lat"], gps["lon"],
                npk["nitrogen"] if npk else 0,
                npk["phosphorus"] if npk else 0,
                npk["potassium"] if npk else 0,
                altitude=gps.get("alt"),
                satellites=gps.get("sats"),
                hdop=gps.get("hdop")
            )
            
            self._event_queue.put({
                "type": "data",
                "payload": {
                    "id": reading_id,
                    "lat": gps["lat"], "lon": gps["lon"],
                    "nitrogen": npk["nitrogen"] if npk else 0,
                    "phosphorus": npk["phosphorus"] if npk else 0,
                    "potassium": npk["potassium"] if npk else 0,
                    "altitude": gps.get("alt"),
                    "satellites": gps.get("sats"),
                    "hdop": gps.get("hdop")
                }
            })
            
            logger.info(f"Auto-captured manual sample to DB (Reading ID: {reading_id})")
        except Exception as e:
            logger.error(f"Failed to auto-capture sample: {e}")

    def drain_events(self):
        """Pop all queued events (called by the API layer, non-blocking)."""
        events = []
        while not self._event_queue.empty():
            events.append(self._event_queue.get_nowait())
        return events

    # ------------------------------------------------------------ simulate mode

    def _simulate_loop(self):
        # Idle loop; actual "hardware" responses happen in _handle_simulated_command
        while self._running:
            time.sleep(0.2)

    def _handle_simulated_command(self, command: str):
        """Simulate the real Arduino Mega behavior with realistic timing.

        The simulation mirrors the actual firmware flow:
        - DRIVE/STOP: immediate ACK
        - SAMPLE: non-blocking actuator cycle (extend 3s → hold/read 5s → retract 3s)
        - GOTO: immediate ACK + status transitions
        - GPS coordinates are generated within the configured field bounds
        """
        def emit(line):
            self._handle_line(line)

        if command.startswith("DRIVE") or command == "STOP":
            emit("ACK " + command.split(" ")[0])
        elif command.startswith("SAMPLE"):
            emit("ACK SAMPLE")

            # Simulate the real actuator cycle with realistic timing
            # Extend phase (3 seconds)
            emit("STATUS LOWERING")
            time.sleep(1.5)  # Shortened for simulation (real = 3s)

            # Hold phase
            emit("STATUS READING")
            time.sleep(1.0)  # Shortened for simulation (real = 5s)

            # Retract phase (3 seconds)
            emit("STATUS RETRACTING")
            time.sleep(0.8)  # Shortened for simulation (real = 3s)

            emit("STATUS IDLE")
        elif command.startswith("GOTO"):
            emit("ACK GOTO")
            emit("STATUS MOVING")
            time.sleep(0.3)
            emit("STATUS ALIGNED")
        elif command == "START_AUTO" or command == "STOP_AUTO":
            emit(f"ACK {command}")
            if command == "STOP_AUTO":
                emit("STATUS IDLE")


# Module-level singleton — imported by routes and sockets.
mega_link = MegaLink()
