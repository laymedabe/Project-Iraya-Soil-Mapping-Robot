"""
Data access layer for Project Iraya.

Deliberately kept as thin wrapper functions around PyMySQL rather than a
full ORM — the query surface is small and fixed, and this keeps resource
usage low on the Pi. Each function opens a short-lived connection; PyMySQL
connections are cheap enough for this request volume (a few dozen writes
per sampling run, not a high-throughput API).
"""

import pymysql
import pymysql.cursors
from contextlib import contextmanager
from app.config import Config


@contextmanager
def get_db():
    conn = pymysql.connect(
        host=Config.DB_HOST,
        port=Config.DB_PORT,
        user=Config.DB_USER,
        password=Config.DB_PASSWORD,
        database=Config.DB_NAME,
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=True,
    )
    try:
        yield conn
    finally:
        conn.close()


# ---------------------------------------------------------------- sessions

def create_session(field_name, lat_min, lat_max, lon_min, lon_max):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """INSERT INTO sessions (field_name, lat_min, lat_max, lon_min, lon_max)
                   VALUES (%s, %s, %s, %s, %s)""",
                (field_name, lat_min, lat_max, lon_min, lon_max),
            )
            return cur.lastrowid


def update_session_status(session_id, status):
    with get_db() as conn:
        with conn.cursor() as cur:
            if status in ("completed", "aborted"):
                cur.execute(
                    "UPDATE sessions SET status=%s, ended_at=NOW() WHERE id=%s",
                    (status, session_id),
                )
            else:
                cur.execute(
                    "UPDATE sessions SET status=%s WHERE id=%s", (status, session_id)
                )


def get_session(session_id):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM sessions WHERE id=%s", (session_id,))
            return cur.fetchone()


def list_sessions(limit=20):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM sessions ORDER BY started_at DESC LIMIT %s", (limit,)
            )
            return cur.fetchall()


# ---------------------------------------------------------------- waypoints

def bulk_insert_waypoints(session_id, waypoints):
    """waypoints: list of (lat, lon) tuples, in path order."""
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.executemany(
                """INSERT INTO waypoints (session_id, seq_index, lat, lon)
                   VALUES (%s, %s, %s, %s)""",
                [(session_id, i, lat, lon) for i, (lat, lon) in enumerate(waypoints)],
            )


def mark_waypoint_visited(waypoint_id):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute("UPDATE waypoints SET visited=1 WHERE id=%s", (waypoint_id,))


def get_next_waypoint(session_id):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """SELECT * FROM waypoints WHERE session_id=%s AND visited=0
                   ORDER BY seq_index ASC LIMIT 1""",
                (session_id,),
            )
            return cur.fetchone()


def get_waypoints(session_id):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM waypoints WHERE session_id=%s ORDER BY seq_index",
                (session_id,),
            )
            return cur.fetchall()


# ---------------------------------------------------------------- readings

def insert_reading(session_id, waypoint_id, lat, lon, n, p, k,
                    moisture=None, temperature=None, ec=None, battery_pct=None):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """INSERT INTO readings
                   (session_id, waypoint_id, lat, lon, nitrogen, phosphorus,
                    potassium, moisture, temperature, ec, battery_pct)
                   VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                (session_id, waypoint_id, lat, lon, n, p, k,
                 moisture, temperature, ec, battery_pct),
            )
            return cur.lastrowid


def get_readings(session_id):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM readings WHERE session_id=%s ORDER BY recorded_at",
                (session_id,),
            )
            return cur.fetchall()


def get_unsynced_readings(limit=500):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT * FROM readings WHERE synced_to_cloud=0 LIMIT %s", (limit,)
            )
            return cur.fetchall()


def mark_readings_synced(reading_ids):
    if not reading_ids:
        return
    with get_db() as conn:
        with conn.cursor() as cur:
            fmt = ",".join(["%s"] * len(reading_ids))
            cur.execute(
                f"UPDATE readings SET synced_to_cloud=1 WHERE id IN ({fmt})",
                tuple(reading_ids),
            )


# ---------------------------------------------------------------- events

def log_event(level, source, message, session_id=None):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """INSERT INTO system_events (session_id, level, source, message)
                   VALUES (%s,%s,%s,%s)""",
                (session_id, level, source, message),
            )
