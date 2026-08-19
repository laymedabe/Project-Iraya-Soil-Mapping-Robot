"""
Shared extension instances.

Kept in their own module (rather than instantiated inside __init__.py)
so other modules — serial_comm.py, sockets.py, routes — can import
`socketio` without triggering circular imports with the app factory.
"""

from flask_socketio import SocketIO

# Standard threading gives us non-blocking serial reads + WebSocket handling
# since eventlet monkey-patching is incompatible with Python 3.13.
socketio = SocketIO(cors_allowed_origins="*")
