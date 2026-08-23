import serial
import threading
import pynmea2
import time
import math
import logging
from app.config import Config

logger = logging.getLogger(__name__)

class GPSReader:
    def __init__(self):
        self.port = Config.GPS_PORT
        self.baud = Config.GPS_BAUD
        self.simulate = Config.GPS_SIMULATE
        self.ser = None
        self._lock = threading.Lock()
        
        self.lat = 0.0
        self.lon = 0.0
        self.alt = 0.0
        self.sats = 0
        self.hdop = 0.0
        self.cog = 0.0
        self.spd = 0.0
        self.fix = False
        
        self.running = False
        self.thread = None
        
        # Simulation state
        self._sim_lat = 14.999
        self._sim_lon = 121.000
        self._sim_heading = 0.0

    def start(self):
        if self.running: return
        self.running = True
        if not self.simulate:
            try:
                self.ser = serial.Serial(self.port, self.baud, timeout=1.0)
                logger.info(f"Opened GPS port: {self.port} at {self.baud} baud")
            except Exception as e:
                logger.error(f"Failed to open GPS port: {e}. Falling back to simulation.")
                self.simulate = True
        
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2.0)
        if self.ser:
            self.ser.close()

    def _read_loop(self):
        while self.running:
            if self.simulate:
                self._update_sim()
                time.sleep(0.2) # 5 Hz sim update
                continue
                
            try:
                line = self.ser.readline().decode('ascii', errors='replace').strip()
                if not line:
                    continue
                if line.startswith('$GPRMC') or line.startswith('$GNRMC'):
                    msg = pynmea2.parse(line)
                    with self._lock:
                        if msg.status == 'A':
                            self.lat = msg.latitude
                            self.lon = msg.longitude
                            self.cog = float(msg.true_course) if msg.true_course else 0.0
                            self.spd = float(msg.spd_over_grnd) * 0.514444 if msg.spd_over_grnd else 0.0 # knots to m/s
                            self.fix = True
                        else:
                            self.fix = False
                elif line.startswith('$GPGGA') or line.startswith('$GNGGA'):
                    msg = pynmea2.parse(line)
                    with self._lock:
                        if msg.gps_qual > 0:
                            self.lat = msg.latitude
                            self.lon = msg.longitude
                            self.alt = float(msg.altitude) if msg.altitude else 0.0
                            self.sats = int(msg.num_sats) if msg.num_sats else 0
                            self.hdop = float(msg.horizontal_dil) if msg.horizontal_dil else 0.0
                            self.fix = True
                        else:
                            self.fix = False
            except serial.SerialException as e:
                logger.error(f"GPS Serial error: {e}")
                time.sleep(1)
            except pynmea2.ParseError:
                pass
            except Exception as e:
                logger.error(f"GPS Parse unexpected error: {e}")

    def _update_sim(self):
        # Move in a slow circle for simulation
        self._sim_heading = (self._sim_heading + 5) % 360
        rad = math.radians(self._sim_heading)
        speed = 0.5 # m/s
        dt = 0.2
        d_lat = (speed * dt * math.cos(rad)) / 111111.0
        d_lon = (speed * dt * math.sin(rad)) / (111111.0 * math.cos(math.radians(self._sim_lat)))
        
        with self._lock:
            self._sim_lat += d_lat
            self._sim_lon += d_lon
            self.lat = self._sim_lat
            self.lon = self._sim_lon
            self.cog = self._sim_heading
            self.spd = speed
            self.alt = 50.0
            self.sats = 8
            self.hdop = 1.0
            self.fix = True

    def get_position(self):
        with self._lock:
            return {
                'lat': self.lat,
                'lon': self.lon,
                'alt': self.alt,
                'sats': self.sats,
                'hdop': self.hdop,
                'cog': self.cog,
                'spd': self.spd,
                'fix': self.fix
            }

gps_reader = GPSReader()
