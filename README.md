# Project Iraya — Autonomous Soil Analysis & Precision Agriculture Robot

## Repository layout

```
project_iraya/
├── raspberry_pi/            Flask + SocketIO application (runs on Pi 5)
│   ├── app/
│   │   ├── __init__.py       App factory
│   │   ├── config.py         Central configuration (serial port, DB creds, etc.)
│   │   ├── extensions.py     SocketIO / shared extension instances
│   │   ├── models.py         MariaDB data access layer
│   │   ├── serial_comm.py    UART bridge to the Arduino Mega
│   │   ├── interpolation.py  IDW soil-nutrient interpolation
│   │   ├── sockets.py        SocketIO events (manual drive control)
│   │   ├── routes/
│   │   │   ├── api.py        REST endpoints (sessions, readings, map data)
│   │   │   └── dashboard.py  HTML page routes
│   │   ├── templates/        Jinja2 templates (dashboard + drive panel)
│   │   └── static/           CSS/JS for the browser dashboard
│   ├── database/schema.sql   MariaDB schema
│   ├── scripts/
│   │   ├── init_db.py        One-time DB bootstrap
│   │   └── sync_to_cloud.py  Push local readings to a cloud DB when online
│   ├── requirements.txt
│   └── run.py                Entry point: `python run.py`
│
├── arduino_mega/project_iraya_mega/   Arduino Mega firmware (PlatformIO or Arduino IDE)
│   ├── project_iraya_mega.ino          Main state machine + serial command loop
│   ├── config.h                        Pin map and tunable constants
│   ├── drive_control.h/.cpp            Skid-steer motor mixing + watchdog
│   ├── actuator_control.h/.cpp         Linear actuator position control
│   └── npk_sensor.h/.cpp               RS485 Modbus RTU NPK sensor driver
│
└── docs/ARCHITECTURE.md      Full design rationale (control hierarchy, safety)
```

## Quick start (Raspberry Pi 5, Raspberry Pi OS Bookworm 64-bit)

```bash
cd raspberry_pi
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# One-time DB setup (requires MariaDB installed and running)
mysql -u root -p < database/schema.sql
python scripts/init_db.py     # verifies connection, seeds a demo session

python run.py
```

Then, on a phone/laptop connected to the Pi's Wi-Fi hotspot, browse to
`http://192.168.4.1:5000` (adjust to your hotspot's actual gateway IP).

## Arduino Mega firmware

Open `arduino_mega/project_iraya_mega/project_iraya_mega.ino` in the Arduino IDE
or PlatformIO. Install the required libraries listed at the top of the `.ino`
file, set the correct COM port, and flash. Pin assignments are centralized in
`config.h` — update them to match your actual wiring before flashing.

## Design principle: layered control

The Raspberry Pi **never drives motors or the actuator directly**. It sends
high-level commands (`DRIVE`, `STOP`, `GOTO`, `SAMPLE`) over UART; the Mega
owns all real-time timing, safety watchdogs, and hardware I/O. See
`docs/ARCHITECTURE.md` for the full rationale.
