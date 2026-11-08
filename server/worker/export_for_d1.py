#!/usr/bin/env python3
"""Turn the Docker server's trials.db into SQL you can paste into the Cloudflare D1 console.

    python3 export_for_d1.py /mnt/user/appdata/trialshd/trials.db > import.sql

Players keep their passwords (only the hashes are copied) and their best times.
"""
import sqlite3
import sys


def lit(v):
    if v is None:
        return "NULL"
    if isinstance(v, (int, float)):
        return repr(v)
    return "'" + str(v).replace("'", "''") + "'"


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    conn = sqlite3.connect(sys.argv[1])
    print("""CREATE TABLE IF NOT EXISTS players (xuid INTEGER PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE NOT NULL, key_hash TEXT NOT NULL, created REAL NOT NULL);
CREATE TABLE IF NOT EXISTS entries (view INTEGER NOT NULL, xuid INTEGER NOT NULL, rating INTEGER NOT NULL, props TEXT NOT NULL, updated REAL NOT NULL, PRIMARY KEY (view, xuid));
CREATE TABLE IF NOT EXISTS submissions (id INTEGER PRIMARY KEY AUTOINCREMENT, view INTEGER NOT NULL, xuid INTEGER NOT NULL, props TEXT NOT NULL, received REAL NOT NULL);
CREATE TABLE IF NOT EXISTS friends (xuid INTEGER NOT NULL, friend INTEGER NOT NULL, PRIMARY KEY (xuid, friend));
CREATE INDEX IF NOT EXISTS entries_rank ON entries (view, rating);""")
    counts = {}
    for table in ("players", "entries", "friends"):
        rows = conn.execute(f"SELECT * FROM {table}").fetchall()
        counts[table] = len(rows)
        for r in rows:
            print(f"INSERT OR REPLACE INTO {table} VALUES ({','.join(lit(v) for v in r)});")
    print(f"-- {counts}", file=sys.stderr)


if __name__ == "__main__":
    main()
