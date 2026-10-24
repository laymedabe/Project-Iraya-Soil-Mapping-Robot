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
from app import state

logger = logging.getLogger("iraya.api")
api_bp = Blueprint("api", __name__, url_prefix="/api")





@api_bp.route("/telemetry", methods=["GET"])
def get_telemetry():
    """Polled by the dashboard every ~1.5s."""
    events = mega_link.drain_events()
    new_readings = []
    
    # Normally we drain events to look for FAULTs and DATA
    for evt in events:
        if evt["type"] == "fault":
            models.log_event("fault", "mega", evt["payload"])
        elif evt["type"] == "data":
            new_readings.append(evt["payload"])
            
    from app.gps_reader import gps_reader
    gps = gps_reader.get_position()

    return jsonify({
        "mega_step": mega_link.state["step"],
        "connected": mega_link.state["connected"],
        "gps": gps,
        "new_readings": new_readings, # No async readings in manual mode
    })


@api_bp.route("/readings", methods=["GET"])
def get_readings():
    return jsonify(models.get_all_readings())


@api_bp.route("/map", methods=["GET"])
def get_map():
    readings = models.get_all_readings()
    if not readings:
        return jsonify({})

    # 1. Filter out uncalibrated / probe-in-air zero dummy readings if valid non-zero data exists
    valid_readings = [
        r for r in readings
        if (float(r.get("nitrogen") or 0) > 0 or 
            float(r.get("phosphorus") or 0) > 0 or 
            float(r.get("potassium") or 0) > 0)
    ]
    used_readings = valid_readings if valid_readings else readings

    # 2. Spatially deduplicate: keep the latest reading per coordinate bin (~11m resolution)
    # This prevents hundreds of repeated bench/stationary zero tests from diluting newer soil measurements
    spatial_dict = {}
    for r in used_readings:
        try:
            lat = float(r["lat"])
            lon = float(r["lon"])
            key = (round(lat, 4), round(lon, 4))
            spatial_dict[key] = r
        except (ValueError, TypeError, KeyError):
            continue

    samples = [
        {
            "lat": float(r["lat"]),
            "lon": float(r["lon"]),
            "nitrogen": float(r["nitrogen"]),
            "phosphorus": float(r["phosphorus"]),
            "potassium": float(r["potassium"]),
        }
        for r in spatial_dict.values()
    ]

    if not samples:
        return jsonify({})

    grid = idw_interpolate(
        samples,
        float(Config.FIELD_LAT_MIN), float(Config.FIELD_LAT_MAX),
        float(Config.FIELD_LON_MIN), float(Config.FIELD_LON_MAX),
        grid_res=40,
    )
    return jsonify(grid)

@api_bp.route("/waypoints", methods=["GET"])
def get_waypoints():
    return jsonify([])


@api_bp.route("/status", methods=["GET"])
def get_status():
    """Global robot status endpoint for mode polling."""
    return jsonify({
        "mode": state.current_mode,
        "auto_task": state.auto_task,
        "mega_step": mega_link.state["step"],
        "mega_connected": mega_link.state["connected"]
    })

@api_bp.route("/sample", methods=["POST"])
def trigger_sample():
    """Manually trigger a sample from the web dashboard."""
    if not mega_link.state["connected"]:
        return jsonify({"error": "Mega not connected"}), 400
    if mega_link.state["step"] not in ["IDLE", "RAISED"]:
        return jsonify({"error": f"Cannot sample while in state {mega_link.state['step']}"}), 400
        
    try:
        mega_link.send_command("SAMPLE")
        return jsonify({"status": "sampling started"})
    except Exception as e:
        logger.error(f"Failed to trigger sample: {e}")
        return jsonify({"error": str(e)}), 500

@api_bp.route("/drive", methods=["POST"])
def drive_robot():
    """Manually drive the robot from the web dashboard."""
    if not mega_link.state["connected"]:
        return jsonify({"error": "Mega not connected"}), 400
    
    data = request.get_json() or {}
    direction = data.get("direction", "STOP").upper()
    speed = data.get("speed", 200)

    if direction not in ["FWD", "BACK", "LEFT", "RIGHT", "STOP"]:
        return jsonify({"error": "Invalid direction"}), 400

    try:
        if direction == "STOP":
            mega_link.send_command("STOP")
        else:
            mega_link.send_command(f"DRIVE {direction} {speed}")
        return jsonify({"status": f"Driving {direction}"})
    except Exception as e:
        logger.error(f"Failed to trigger drive: {e}")
        return jsonify({"error": str(e)}), 500

@api_bp.route("/export/csv", methods=["GET"])
def export_csv():
    """Export all readings as a CSV file."""
    import io
    import csv
    from flask import Response
    
    readings = models.get_all_readings()
    
    output = io.StringIO()
    writer = csv.writer(output)
    
    # Write header
    writer.writerow([
        "ID", "Time", "Latitude", "Longitude", 
        "Nitrogen (mg/kg)", "Phosphorus (mg/kg)", "Potassium (mg/kg)", 
        "Moisture (%)", "Temperature (C)", "EC (dS/m)", "pH",
        "Altitude (m)", "Satellites", "HDOP"
    ])
    
    # Write data
    for r in readings:
        writer.writerow([
            r.get("id"),
            r.get("recorded_at"),
            r.get("lat"),
            r.get("lon"),
            r.get("nitrogen"),
            r.get("phosphorus"),
            r.get("potassium"),
            r.get("moisture"),
            r.get("temperature"),
            r.get("ec"),
            r.get("ph"),
            r.get("altitude"),
            r.get("satellites"),
            r.get("hdop")
        ])
        
    return Response(
        output.getvalue(),
        mimetype="text/csv",
        headers={"Content-disposition": "attachment; filename=iraya_readings.csv"}
    )


@api_bp.route("/cleanup_dummies", methods=["GET", "POST"])
def cleanup_dummies():
    """Purges all-zero timeout dummy records logged when the sensor was disconnected/unpowered."""
    try:
        with models.get_db() as conn:
            with conn.cursor() as cur:
                cur.execute("""
                    DELETE FROM readings 
                    WHERE lat = 0 AND lon = 0 AND moisture = 0 AND temperature = 0 AND ec = 0 AND ph = 0 AND nitrogen = 0
                """)
        return jsonify({"status": "success", "message": "Deleted all-zero dummy readings from database"})
    except Exception as e:
        logger.error(f"Failed to cleanup dummy readings: {e}")
        return jsonify({"error": str(e)}), 500


@api_bp.route("/gps/override", methods=["POST"])
def gps_override():
    """Manually inject GPS coordinates when the module has no fix (e.g. when moving to a new site)."""
    data = request.get_json() or {}
    try:
        lat = float(data["lat"])
        lon = float(data["lon"])
    except (KeyError, ValueError, TypeError):
        return jsonify({"error": "lat and lon are required numeric fields"}), 400

    from app.gps_reader import gps_reader
    import time
    with gps_reader._lock:
        gps_reader.lat = lat
        gps_reader.lon = lon
        gps_reader.fix = True
        gps_reader._last_fix_time = time.time()
        # Tag so the telemetry UI can show "Manual" instead of satellite count
        gps_reader._manual_override = True

    logger.info(f"GPS manual override applied: lat={lat}, lon={lon}")
    return jsonify({"status": "ok", "lat": lat, "lon": lon})


