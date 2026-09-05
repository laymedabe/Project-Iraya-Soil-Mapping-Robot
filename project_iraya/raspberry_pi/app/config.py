"""
Central configuration for Project Iraya's Flask application.

All environment-specific values (serial port, DB credentials, etc.) are
read from environment variables so the same codebase runs unmodified on
a developer's laptop (simulated serial) and the actual Raspberry Pi.
"""

import os
from dotenv import load_dotenv

load_dotenv()


class Config:
    # --- Flask ---
    SECRET_KEY = os.environ.get("IRAYA_SECRET_KEY", "dev-key-change-in-production")
    DEBUG = os.environ.get("IRAYA_DEBUG", "false").lower() == "true"

    # --- Serial link to Arduino Mega ---
    SERIAL_PORT = os.environ.get("IRAYA_SERIAL_PORT", "/dev/ttyUSB0")
    SERIAL_BAUD = int(os.environ.get("IRAYA_SERIAL_BAUD", "9600"))
    SERIAL_TIMEOUT = float(os.environ.get("IRAYA_SERIAL_TIMEOUT", "0.1"))
    # Set to true on a dev machine with no Mega attached — serial_comm.py
    # will log commands instead of writing to a real port.
    SERIAL_SIMULATE = os.environ.get("IRAYA_SERIAL_SIMULATE", "false").lower() == "true"

    # --- GPS (NEO-M8) ---
    GPS_PORT = os.environ.get("IRAYA_GPS_PORT", "/dev/ttyAMA0")
    GPS_BAUD = int(os.environ.get("IRAYA_GPS_BAUD", "9600"))
    GPS_SIMULATE = os.environ.get("IRAYA_GPS_SIMULATE", "false").lower() == "true"

    # --- NPK Sensor (RS485 Modbus TTL) ---
    NPK_PORT = os.environ.get("IRAYA_NPK_PORT", "/dev/ttyUSB0")
    NPK_BAUD = int(os.environ.get("IRAYA_NPK_BAUD", "9600"))
    NPK_SIMULATE = os.environ.get("IRAYA_NPK_SIMULATE", "false").lower() == "true"
    NPK_DERE_PIN = int(os.environ.get("IRAYA_NPK_DERE_PIN", "17")) # GPIO 17 / Pin 11

    # --- Drive safety ---
    DRIVE_HEARTBEAT_TIMEOUT_S = 0.5   # server-side: stop if no heartbeat in this window

    # --- Database (MariaDB on Pi 5 / SQLite for zero-config dev) ---
    DB_ENGINE = os.environ.get("IRAYA_DB_ENGINE", "sqlite" if os.name == "nt" else "mariadb").lower()
    DB_FILE = os.environ.get("IRAYA_DB_FILE", os.path.join(os.path.dirname(__file__), "..", "project_iraya.db"))
    DB_HOST = os.environ.get("IRAYA_DB_HOST", "localhost")
    DB_PORT = int(os.environ.get("IRAYA_DB_PORT", "3306"))
    DB_USER = os.environ.get("IRAYA_DB_USER", "iraya")
    DB_PASSWORD = os.environ.get("IRAYA_DB_PASSWORD", "changeme")
    DB_NAME = os.environ.get("IRAYA_DB_NAME", "project_iraya")

    # --- Field bounding box (used for IDW grid extents) ---
    FIELD_LAT_MIN = float(os.environ.get("IRAYA_LAT_MIN", "14.997"))
    FIELD_LAT_MAX = float(os.environ.get("IRAYA_LAT_MAX", "15.001"))
    FIELD_LON_MIN = float(os.environ.get("IRAYA_LON_MIN", "120.998"))
    FIELD_LON_MAX = float(os.environ.get("IRAYA_LON_MAX", "121.003"))
