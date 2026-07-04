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
            "last_data": None,     # most recent parsed DATA reading (dict)
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
        if line.startswith("STATUS"):
            self.state["step"] = line.split(" ", 1)[1] if " " in line else line
        elif line.startswith("DATA"):
            self.state["last_data"] = self._parse_data_line(line)
            self._event_queue.put({"type": "data", "payload": self.state["last_data"]})
        elif line.startswith("FAULT"):
            self.state["last_fault"] = line
            self._event_queue.put({"type": "fault", "payload": line})
        elif line.startswith("ACK"):
            pass  # command acknowledged, nothing to do beyond logging

    @staticmethod
    def _parse_data_line(line: str) -> dict:
        # "DATA N=42.3 P=15.1 K=118.6 MOIST=21.4 TEMP=27.1 EC=0.8"
        fields = line.split(" ")[1:]
        result = {}
        key_map = {
            "N": "nitrogen", "P": "phosphorus", "K": "potassium",
            "MOIST": "moisture", "TEMP": "temperature", "EC": "ec",
        }
        for f in fields:
            if "=" not in f:
                continue
            k, v = f.split("=")
            result[key_map.get(k, k.lower())] = float(v)
        return result

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
        def emit(line):
            self._handle_line(line)

        if command.startswith("DRIVE") or command == "STOP":
            emit("ACK " + command.split(" ")[0])
        elif command.startswith("SAMPLE"):
            emit("ACK SAMPLE")
            for step, delay in [("LOWERING", 0.3), ("READING", 0.4), ("RAISED", 0.2)]:
                time.sleep(delay)
                emit(f"STATUS {step}")
            n = round(20 + random.random() * 60, 1)
            p = round(8 + random.random() * 30, 1)
            k = round(60 + random.random() * 120, 1)
            moist = round(15 + random.random() * 25, 1)
            temp = round(25 + random.random() * 6, 1)
            ec = round(0.4 + random.random() * 1.2, 2)
            emit(f"DATA N={n} P={p} K={k} MOIST={moist} TEMP={temp} EC={ec}")
        elif command.startswith("GOTO"):
            emit("ACK GOTO")
            emit("STATUS MOVING")
            time.sleep(0.3)
            emit("STATUS ALIGNED")


# Module-level singleton — imported by routes and sockets.
mega_link = MegaLink()
