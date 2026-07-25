"""
SocketIO handlers — manual drive control.

Safety notes
------------
This is the *convenience* stop path, not the primary safety guarantee.
The Arduino Mega runs its own 400ms watchdog independent of this code
(see arduino_mega/project_iraya_mega/drive_control.cpp). If this process
crashes entirely, the Mega still stops itself. What this module adds:

1. A server-side heartbeat check so a stuck browser tab that stops sending
   events (without a clean disconnect) still results in a STOP being sent.
2. An immediate STOP the instant the WebSocket disconnects, which is
   typically faster than waiting for the Mega's own timeout.
"""

import logging
import time
import threading
from flask_socketio import emit
from app.extensions import socketio
from app.serial_comm import mega_link
from app.config import Config
from app import models
from app import state

logger = logging.getLogger("iraya.sockets")

_last_heartbeat = {"ts": 0.0, "sid": None}
_watchdog_started = False


def _watchdog_loop():
    """Belt-and-suspenders server-side watchdog, independent of the Mega's own."""
    while True:
        time.sleep(0.2)
        if _last_heartbeat["sid"] and (
            time.time() - _last_heartbeat["ts"] > Config.DRIVE_HEARTBEAT_TIMEOUT_S
        ):
            try:
                mega_link.send_command("STOP")
            except Exception as exc:
                logger.error(f"Watchdog STOP failed: {exc}")
            _last_heartbeat["sid"] = None


def _ensure_watchdog():
    global _watchdog_started
    if not _watchdog_started:
        threading.Thread(target=_watchdog_loop, daemon=True).start()
        _watchdog_started = True


@socketio.on("connect")
def handle_connect():
    _ensure_watchdog()
    logger.info("Drive client connected")
    emit("mega_state", mega_link.state)


@socketio.on("drive")
def handle_drive(data):
    """
    data = {"direction": "FWD" | "BACK" | "LEFT" | "RIGHT" | "STOP", "speed": 0-255}
    Sent repeatedly (~150ms) by the browser while a direction is held.
    """
    with state.mode_lock:
        if state.current_mode == "auto":
            emit("mode_conflict", {"message": "Auto Run is active — drive controls locked."})
            return

    direction = data.get("direction", "STOP")
    speed = max(0, min(255, int(data.get("speed", 0))))

    try:
        if direction == "STOP":
            mega_link.send_command("STOP")
            _last_heartbeat["sid"] = None
        else:
            mega_link.send_command(f"DRIVE {direction} {speed}")
            _last_heartbeat["ts"] = time.time()
            from flask import request
            _last_heartbeat["sid"] = request.sid
    except Exception as exc:
        logger.error(f"Drive command failed: {exc}")
        models.log_event("fault", "sockets", f"Drive command failed: {exc}")
        emit("drive_error", {"message": str(exc)})


@socketio.on("disconnect")
def handle_disconnect():
    logger.warning("Drive client disconnected — sending STOP")
    try:
        mega_link.send_command("STOP")
    except Exception as exc:
        logger.error(f"Disconnect STOP failed: {exc}")
    _last_heartbeat["sid"] = None
