"""
Global state tracker for Project Iraya.
Used to enforce mutually exclusive operating modes (AUTO vs MANUAL).
"""

import threading

# "idle", "auto", or "manual"
current_mode = "idle"

# details about the current auto run
auto_task = {
    "session_id": None,
    "current_waypoint_index": 0,
    "total_waypoints": 0,
}

# Lock for state transitions
mode_lock = threading.Lock()
