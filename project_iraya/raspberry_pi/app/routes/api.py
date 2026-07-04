"""
REST API for Project Iraya's dashboard.

Endpoints
---------
POST /api/session/start     Start a new sampling session, generate waypoints
POST /api/session/<id>/stop Mark a session complete/aborted
GET  /api/session/<id>       Session metadata
GET  /api/session/<id>/latest  Drains queued Mega events, returns current state
POST /api/session/<id>/sample  Advance to next waypoint and trigger a sample
GET  /api/session/<id>/readings  All readings for a session
GET  /api/session/<id>/map      IDW-interpolated nutrient grid
"""

import logging
from flask import Blueprint, jsonify, request
from app import models
from app.serial_comm import mega_link
from app.interpolation import idw_interpolate
from app.config import Config

logger = logging.getLogger("iraya.api")
api_bp = Blueprint("api", __name__, url_prefix="/api")


def _generate_boustrophedon_path(lat_min, lat_max, lon_min, lon_max, rows=4, cols=5):
    """Back-and-forth sampling path — matches the field-map layout used
    in the dashboard prototype and minimizes travel distance vs. a
    raster (always-left-to-right) pattern."""
    points = []
    for r in range(rows):
        row_cols = range(cols) if r % 2 == 0 else reversed(range(cols))
        lat = lat_max - (r / max(rows - 1, 1)) * (lat_max - lat_min)
        for c in row_cols:
            lon = lon_min + (c / max(cols - 1, 1)) * (lon_max - lon_min)
            points.append((lat, lon))
    return points


@api_bp.route("/session/start", methods=["POST"])
def start_session():
    body = request.get_json(silent=True) or {}
    field_name = body.get("field_name", "Unnamed Field")
    lat_min = body.get("lat_min", Config.FIELD_LAT_MIN)
    lat_max = body.get("lat_max", Config.FIELD_LAT_MAX)
    lon_min = body.get("lon_min", Config.FIELD_LON_MIN)
    lon_max = body.get("lon_max", Config.FIELD_LON_MAX)
    rows = int(body.get("rows", 4))
    cols = int(body.get("cols", 5))

    session_id = models.create_session(field_name, lat_min, lat_max, lon_min, lon_max)
    path = _generate_boustrophedon_path(lat_min, lat_max, lon_min, lon_max, rows, cols)
    models.bulk_insert_waypoints(session_id, path)
    models.log_event("info", "api", f"Session {session_id} started ({len(path)} waypoints)", session_id)

    return jsonify({"session_id": session_id, "waypoint_count": len(path)}), 201


@api_bp.route("/session/<int:session_id>/stop", methods=["POST"])
def stop_session(session_id):
    status = request.get_json(silent=True, force=True) or {}
    final_status = status.get("status", "completed")
    if final_status not in ("completed", "aborted"):
        final_status = "completed"
    models.update_session_status(session_id, final_status)
    try:
        mega_link.send_command("STOP")
    except Exception as exc:
        logger.error(f"Failed to send STOP on session stop: {exc}")
    models.log_event("info", "api", f"Session {session_id} {final_status}", session_id)
    return jsonify({"status": final_status})


@api_bp.route("/session/<int:session_id>", methods=["GET"])
def get_session(session_id):
    session = models.get_session(session_id)
    if not session:
        return jsonify({"error": "not found"}), 404
    return jsonify(session)


@api_bp.route("/sessions", methods=["GET"])
def list_sessions():
    return jsonify(models.list_sessions())


@api_bp.route("/session/<int:session_id>/sample", methods=["POST"])
def trigger_sample(session_id):
    """Advances to the next unvisited waypoint, sends GOTO + SAMPLE to the
    Mega. This endpoint returns immediately (ack only) — the actual reading
    arrives asynchronously via the Mega's DATA line and is fetched by the
    client through /latest."""
    waypoint = models.get_next_waypoint(session_id)
    if waypoint is None:
        return jsonify({"done": True, "message": "All waypoints visited"}), 200

    try:
        mega_link.send_command(f"GOTO {waypoint['lat']} {waypoint['lon']}")
        mega_link.send_command("SAMPLE")
    except Exception as exc:
        logger.error(f"Sample command failed: {exc}")
        models.log_event("fault", "api", f"Sample command failed: {exc}", session_id)
        return jsonify({"error": str(exc)}), 500

    return jsonify({
        "done": False,
        "waypoint_id": waypoint["id"],
        "lat": float(waypoint["lat"]),
        "lon": float(waypoint["lon"]),
    })


@api_bp.route("/session/<int:session_id>/latest", methods=["GET"])
def latest_state(session_id):
    """Polled by the dashboard every ~1s. Drains any DATA events produced
    since the last poll, persists them, and returns current Mega state."""
    events = mega_link.drain_events()
    new_readings = []

    for evt in events:
        if evt["type"] == "data":
            payload = evt["payload"]
            waypoint = models.get_next_waypoint(session_id)  # the one just completed
            # NOTE: in a fuller implementation, track "in-flight" waypoint id
            # explicitly rather than re-querying; simplified here for clarity.
            wp_id = waypoint["id"] if waypoint else None
            lat = float(waypoint["lat"]) if waypoint else Config.FIELD_LAT_MIN
            lon = float(waypoint["lon"]) if waypoint else Config.FIELD_LON_MIN

            reading_id = models.insert_reading(
                session_id, wp_id, lat, lon,
                payload.get("nitrogen"), payload.get("phosphorus"), payload.get("potassium"),
                payload.get("moisture"), payload.get("temperature"), payload.get("ec"),
            )
            if wp_id:
                models.mark_waypoint_visited(wp_id)
            new_readings.append({"id": reading_id, **payload, "lat": lat, "lon": lon})
        elif evt["type"] == "fault":
            models.log_event("fault", "mega", evt["payload"], session_id)

    return jsonify({
        "mega_step": mega_link.state["step"],
        "connected": mega_link.state["connected"],
        "new_readings": new_readings,
    })


@api_bp.route("/session/<int:session_id>/readings", methods=["GET"])
def get_readings(session_id):
    return jsonify(models.get_readings(session_id))


@api_bp.route("/session/<int:session_id>/map", methods=["GET"])
def get_map(session_id):
    session = models.get_session(session_id)
    if not session:
        return jsonify({"error": "not found"}), 404

    readings = models.get_readings(session_id)
    samples = [
        {
            "lat": float(r["lat"]), "lon": float(r["lon"]),
            "nitrogen": float(r["nitrogen"]), "phosphorus": float(r["phosphorus"]),
            "potassium": float(r["potassium"]),
        }
        for r in readings
    ]

    grid = idw_interpolate(
        samples,
        float(session["lat_min"]), float(session["lat_max"]),
        float(session["lon_min"]), float(session["lon_max"]),
        grid_res=40,
    )
    return jsonify(grid)


@api_bp.route("/session/<int:session_id>/waypoints", methods=["GET"])
def get_waypoints(session_id):
    return jsonify(models.get_waypoints(session_id))
