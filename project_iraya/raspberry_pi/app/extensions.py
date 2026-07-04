"""
Shared extension instances.

Kept in their own module (rather than instantiated inside __init__.py)
so other modules — serial_comm.py, sockets.py, routes — can import
`socketio` without triggering circular imports with the app factory.
"""

from flask_socketio import SocketIO

# eventlet gives us non-blocking serial reads + WebSocket handling on a
# single Pi core without needing a separate process/queue broker.
socketio = SocketIO(async_mode="eventlet", cors_allowed_origins="*")
