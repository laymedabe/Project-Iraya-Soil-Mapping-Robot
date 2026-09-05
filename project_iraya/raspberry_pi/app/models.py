"""
Data access layer for Project Iraya.

Supports both MariaDB (production on Raspberry Pi 5) and SQLite
(zero-config development on Windows / developer laptops).
"""

import sqlite3
import logging
from contextlib import contextmanager
from app.config import Config

logger = logging.getLogger("iraya.models")

try:
    import pymysql
    import pymysql.cursors
except ImportError:
    pymysql = None

_sqlite_initialized = False


def _init_sqlite_tables(conn):
    global _sqlite_initialized
    if _sqlite_initialized:
        return
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE IF NOT EXISTS sessions (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            field_name TEXT NOT NULL DEFAULT 'Unnamed Field',
            started_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            ended_at DATETIME NULL,
            status TEXT NOT NULL DEFAULT 'running',
            lat_min REAL, lat_max REAL,
            lon_min REAL, lon_max REAL,
            notes TEXT NULL,
            synced_to_cloud INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE IF NOT EXISTS waypoints (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER NOT NULL,
            seq_index INTEGER NOT NULL,
            lat REAL NOT NULL,
            lon REAL NOT NULL,
            visited INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE
        );

        CREATE TABLE IF NOT EXISTS readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER NOT NULL,
            waypoint_id INTEGER NULL,
            recorded_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            lat REAL NOT NULL,
            lon REAL NOT NULL,
            nitrogen REAL NOT NULL,
            phosphorus REAL NOT NULL,
            potassium REAL NOT NULL,
            moisture REAL NULL,
            temperature REAL NULL,
            ec REAL NULL,
            ph REAL NULL,
            altitude REAL NULL,
            satellites INTEGER NULL,
            hdop REAL NULL,
            battery_pct REAL NULL,
            synced_to_cloud INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE CASCADE,
            FOREIGN KEY (waypoint_id) REFERENCES waypoints(id) ON DELETE SET NULL
        );

        CREATE TABLE IF NOT EXISTS system_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_id INTEGER NULL,
            occurred_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
            level TEXT NOT NULL DEFAULT 'info',
            source TEXT NOT NULL,
            message TEXT NOT NULL,
            FOREIGN KEY (session_id) REFERENCES sessions(id) ON DELETE SET NULL
        );
    """)
    conn.commit()
    _sqlite_initialized = True


class SQLiteCursorWrapper:
    def __init__(self, conn):
        self._conn = conn
        self._cur = conn.cursor()
        self.lastrowid = None

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self._cur.close()

    def _format_query(self, query):
        # Replace MySQL placeholders (%s) with SQLite placeholders (?)
        # and NOW() with CURRENT_TIMESTAMP
        q = query.replace("%s", "?").replace("NOW()", "CURRENT_TIMESTAMP")
        return q

    def execute(self, query, params=None):
        q = self._format_query(query)
        if params is None:
            self._cur.execute(q)
        else:
            self._cur.execute(q, params)
        self.lastrowid = self._cur.lastrowid
        return self

    def executemany(self, query, seq_of_params):
        q = self._format_query(query)
        self._cur.executemany(q, seq_of_params)
        self.lastrowid = self._cur.lastrowid
        return self

    def fetchone(self):
        row = self._cur.fetchone()
        return dict(row) if row else None

    def fetchall(self):
        rows = self._cur.fetchall()
        return [dict(r) for r in rows]


class SQLiteConnWrapper:
    def __init__(self, conn):
        self._conn = conn

    @contextmanager
    def cursor(self):
        yield SQLiteCursorWrapper(self._conn)

    def close(self):
        self._conn.close()


@contextmanager
def get_db():
    use_sqlite = Config.DB_ENGINE == "sqlite"

    if not use_sqlite and pymysql is not None:
        try:
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
            return
        except Exception as exc:
            logger.warning(f"MariaDB connection failed ({exc}). Falling back to SQLite for local development.")
            use_sqlite = True

    if use_sqlite:
        conn = sqlite3.connect(Config.DB_FILE)
        conn.row_factory = sqlite3.Row
        _init_sqlite_tables(conn)
        wrapper = SQLiteConnWrapper(conn)
        try:
            yield wrapper
        finally:
            conn.commit()
            wrapper.close()


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

def get_or_create_default_session():
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT id FROM sessions WHERE field_name='Manual Spot Checks' LIMIT 1")
            row = cur.fetchone()
            if row: return row['id']
            cur.execute("INSERT INTO sessions (field_name) VALUES ('Manual Spot Checks')")
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
                    moisture=None, temperature=None, ec=None, ph=None,
                    altitude=None, satellites=None, hdop=None,
                    battery_pct=None):
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """INSERT INTO readings
                   (session_id, waypoint_id, lat, lon, nitrogen, phosphorus,
                    potassium, moisture, temperature, ec, ph,
                    altitude, satellites, hdop, battery_pct)
                   VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)""",
                (session_id, waypoint_id, lat, lon, n, p, k,
                 moisture, temperature, ec, ph,
                 altitude, satellites, hdop, battery_pct),
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

def get_all_readings():
    with get_db() as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT * FROM readings ORDER BY recorded_at")
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
