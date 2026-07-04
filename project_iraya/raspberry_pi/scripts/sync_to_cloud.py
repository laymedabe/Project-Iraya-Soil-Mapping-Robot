"""
Pushes unsynced readings from the local MariaDB to a cloud database
(e.g. PlanetScale/Aiven) whenever the Pi has internet connectivity.

Intended to run as a cron job on the Pi, e.g. every 15 minutes:
    */15 * * * * /path/to/venv/bin/python /path/to/scripts/sync_to_cloud.py

This is intentionally decoupled from the Flask app — the dashboard must
keep working over the Pi's local hotspot with zero internet, and sync
should never block or slow down a live sampling session.
"""

import os
import sys
import logging
import pymysql

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app import models  # noqa: E402

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger("iraya.sync")

CLOUD_HOST = os.environ.get("IRAYA_CLOUD_DB_HOST")
CLOUD_USER = os.environ.get("IRAYA_CLOUD_DB_USER")
CLOUD_PASSWORD = os.environ.get("IRAYA_CLOUD_DB_PASSWORD")
CLOUD_DB_NAME = os.environ.get("IRAYA_CLOUD_DB_NAME", "project_iraya")


def get_cloud_connection():
    return pymysql.connect(
        host=CLOUD_HOST, user=CLOUD_USER, password=CLOUD_PASSWORD,
        database=CLOUD_DB_NAME, cursorclass=pymysql.cursors.DictCursor,
        ssl={"ssl": {}},  # most managed MySQL/MariaDB providers require TLS
        autocommit=True,
    )


def sync():
    if not CLOUD_HOST:
        logger.warning("IRAYA_CLOUD_DB_HOST not set — skipping sync.")
        return

    unsynced = models.get_unsynced_readings()
    if not unsynced:
        logger.info("Nothing to sync.")
        return

    try:
        cloud_conn = get_cloud_connection()
    except Exception as exc:
        logger.warning(f"Cloud unreachable, will retry next run: {exc}")
        return

    synced_ids = []
    with cloud_conn:
        with cloud_conn.cursor() as cur:
            for r in unsynced:
                try:
                    cur.execute(
                        """INSERT INTO readings
                           (id, session_id, waypoint_id, recorded_at, lat, lon,
                            nitrogen, phosphorus, potassium, moisture, temperature,
                            ec, battery_pct, synced_to_cloud)
                           VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,1)
                           ON DUPLICATE KEY UPDATE synced_to_cloud=1""",
                        (r["id"], r["session_id"], r["waypoint_id"], r["recorded_at"],
                         r["lat"], r["lon"], r["nitrogen"], r["phosphorus"], r["potassium"],
                         r["moisture"], r["temperature"], r["ec"], r["battery_pct"]),
                    )
                    synced_ids.append(r["id"])
                except Exception as exc:
                    logger.error(f"Failed to sync reading {r['id']}: {exc}")

    models.mark_readings_synced(synced_ids)
    logger.info(f"Synced {len(synced_ids)} of {len(unsynced)} readings.")


if __name__ == "__main__":
    sync()
