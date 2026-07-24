"""
Verifies the database connection and schema.
Run once after setup: python scripts/init_db.py
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.models import get_db  # noqa: E402
from app.config import Config  # noqa: E402


def main():
    try:
        with get_db() as conn:
            with conn.cursor() as cur:
                if Config.DB_ENGINE == "sqlite":
                    cur.execute("SELECT name FROM sqlite_master WHERE type='table'")
                    rows = cur.fetchall()
                    tables = [r["name"] for r in rows]
                else:
                    cur.execute("SHOW TABLES")
                    rows = cur.fetchall()
                    tables = [list(r.values())[0] for r in rows]

        expected = {"sessions", "waypoints", "readings", "system_events"}
        missing = expected - set(tables)
        if missing:
            print(f"Missing tables: {missing}.")
            sys.exit(1)
        print(f"Database ready! Engine: {Config.DB_ENGINE}. Found tables: {tables}")
    except Exception as exc:
        print(f"Database connection failed: {exc}")
        sys.exit(1)


if __name__ == "__main__":
    main()
