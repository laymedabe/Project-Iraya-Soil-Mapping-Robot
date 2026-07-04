"""
Verifies the MariaDB connection and schema after running schema.sql.
Run once after setup: python scripts/init_db.py
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from app.models import get_db  # noqa: E402


def main():
    try:
        with get_db() as conn:
            with conn.cursor() as cur:
                cur.execute("SHOW TABLES")
                tables = [list(row.values())[0] for row in cur.fetchall()]
        expected = {"sessions", "waypoints", "readings", "system_events"}
        missing = expected - set(tables)
        if missing:
            print(f"Missing tables: {missing}. Did you run schema.sql?")
            sys.exit(1)
        print(f"Connected OK. Found tables: {tables}")
    except Exception as exc:
        print(f"Database connection failed: {exc}")
        print("Check IRAYA_DB_HOST / IRAYA_DB_USER / IRAYA_DB_PASSWORD in your .env")
        sys.exit(1)


if __name__ == "__main__":
    main()
