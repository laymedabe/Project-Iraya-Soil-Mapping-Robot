"""HTML page routes — the browser-facing dashboard and drive panel."""

from flask import Blueprint, render_template
from app.config import Config

dashboard_bp = Blueprint("dashboard", __name__)


@dashboard_bp.route("/")
def index():
    return render_template("dashboard.html", field_bounds={
        "lat_min": Config.FIELD_LAT_MIN, "lat_max": Config.FIELD_LAT_MAX,
        "lon_min": Config.FIELD_LON_MIN, "lon_max": Config.FIELD_LON_MAX,
    })


@dashboard_bp.route("/drive")
def drive():
    return render_template("drive.html")
