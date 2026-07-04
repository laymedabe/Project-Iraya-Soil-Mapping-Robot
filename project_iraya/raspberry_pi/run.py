"""
Project Iraya — entry point.

Run with:  python run.py
On the Pi, this is normally wrapped by a systemd service so the dashboard
comes up automatically on boot — see docs/ARCHITECTURE.md for a suggested
unit file if you want that set up next.
"""

from app import create_app
from app.extensions import socketio

app = create_app()

if __name__ == "__main__":
    # host=0.0.0.0 so the dashboard is reachable from any device connected
    # to the Pi's Wi-Fi hotspot, not just localhost.
    socketio.run(app, host="0.0.0.0", port=5000, debug=app.config["DEBUG"])
