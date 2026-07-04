"""
Flask application factory for Project Iraya.
"""

import logging
from flask import Flask
from app.config import Config
from app.extensions import socketio
from app.serial_comm import mega_link


def create_app():
    app = Flask(__name__)
    app.config.from_object(Config)

    logging.basicConfig(
        level=logging.DEBUG if Config.DEBUG else logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )

    # Register blueprints
    from app.routes.api import api_bp
    from app.routes.dashboard import dashboard_bp
    app.register_blueprint(api_bp)
    app.register_blueprint(dashboard_bp)

    # SocketIO handlers register themselves via decorators on import
    import app.sockets  # noqa: F401

    socketio.init_app(app)

    # Start the Mega serial link once, at process startup
    mega_link.start()

    return app
