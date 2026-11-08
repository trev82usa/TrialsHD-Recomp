#!/usr/bin/env python3
"""Trials HD leaderboard server: stores times sent by the recompiled game and serves them back
in place of Xbox Live leaderboards.

Standard library only (Python 3.10+). Data lives in one SQLite file.

Environment:
  TRIALS_DB        path to the SQLite database   (default /data/trials.db)
  TRIALS_PORT      port to listen on              (default 8080)
  TRIALS_CONFIG    path to views.json (optional)  (default /data/views.json)
  TRIALS_ADMIN_KEY if set, enables /api/v1/admin/* endpoints with header X-Admin-Key

Players are registered on first use: the first request for a name stores a hash of the player's
key; later requests for that name must send the same key (header X-Player-Key).

How a view is ranked comes from views.json ({"<view id>": {"rating": <property id>,
"order": "asc"|"desc"}}). Views not listed are ranked by their first written property, ascending
(lowest time first), and the server logs what it saw so the config can be filled in.
"""
import hashlib
import hmac
import json
import logging
import os
import re
import sqlite3
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

DB_PATH = os.environ.get("TRIALS_DB", "/data/trials.db")
PORT = int(os.environ.get("TRIALS_PORT", "8080"))
CONFIG_PATH = os.environ.get("TRIALS_CONFIG", "/data/views.json")
ADMIN_KEY = os.environ.get("TRIALS_ADMIN_KEY", "")
# Trials HD writes its ranking value as property 0x20000006 = -(faults << 32 | time in ms), so a
# higher value is better (fewer faults, then faster).
TRIALS_RATING = 0x20000006
NAME_RE = re.compile(r"^[A-Za-z0-9 _\-]{1,15}$")  # Xbox gamertags: 15 chars, ASCII
MAX_BODY = 64 * 1024

log = logging.getLogger("trials")
_db_lock = threading.RLock()  # one connection, serialised


def xuid_for(name: str) -> int:
    """Stable fake XUID for a player name (must match the game client's calculation)."""
    h = 0xCBF29CE484222325
    for b in name.lower().encode():
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    # Keep clear of bits 54-55 (some titles reject those) and mark as an "online" XUID.
    return 0x0009000000000000 | (h & 0x0000FFFFFFFFFFFF)


def load_config() -> dict:
    try:
        with open(CONFIG_PATH) as f:
            return {int(k): v for k, v in json.load(f).items()}
    except FileNotFoundError:
        return {}
    except Exception as e:  # bad JSON should not take the server down
        log.error("could not read %s: %s", CONFIG_PATH, e)
        return {}


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS players (
            xuid INTEGER PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE NOT NULL,
            key_hash TEXT NOT NULL, created REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS entries (
            view INTEGER NOT NULL, xuid INTEGER NOT NULL, rating INTEGER NOT NULL,
            props TEXT NOT NULL, updated REAL NOT NULL, PRIMARY KEY (view, xuid));
        CREATE TABLE IF NOT EXISTS submissions (
            id INTEGER PRIMARY KEY AUTOINCREMENT, view INTEGER NOT NULL, xuid INTEGER NOT NULL,
            props TEXT NOT NULL, received REAL NOT NULL);
        CREATE TABLE IF NOT EXISTS friends (
            xuid INTEGER NOT NULL, friend INTEGER NOT NULL, PRIMARY KEY (xuid, friend));
        CREATE INDEX IF NOT EXISTS entries_rank ON entries (view, rating);
        """
    )
    conn.commit()


def hash_key(key: str) -> str:
    return hashlib.sha256(("trialshd:" + key).encode()).hexdigest()


class ApiError(Exception):
    def __init__(self, status: int, message: str):
        super().__init__(message)
        self.status = status


class Store:
    def __init__(self, conn: sqlite3.Connection):
        self.conn = conn

    # -- players ---------------------------------------------------------------------------
    def authenticate(self, name: str, key: str) -> int:
        if not name or not NAME_RE.match(name):
            raise ApiError(400, "player name must be 1-15 letters, digits, spaces, _ or -")
        if len(key) < 8:
            raise ApiError(400, "player key must be at least 8 characters")
        xuid = xuid_for(name)
        with _db_lock:
            row = self.conn.execute("SELECT name, key_hash FROM players WHERE xuid=?",
                                    (xuid,)).fetchone()
            if row is None:
                self.conn.execute("INSERT INTO players VALUES (?,?,?,?)",
                                  (xuid, name, hash_key(key), time.time()))
                self.conn.commit()
                log.info("registered new player %r (xuid %016X)", name, xuid)
                return xuid
        if not hmac.compare_digest(row["key_hash"], hash_key(key)):
            raise ApiError(403, "wrong key for this player name")
        return xuid

    def names(self, xuids) -> dict:
        xuids = list(xuids)
        if not xuids:
            return {}
        q = ",".join("?" * len(xuids))
        rows = self.conn.execute(f"SELECT xuid, name FROM players WHERE xuid IN ({q})", xuids)
        return {r["xuid"]: r["name"] for r in rows}

    # -- writing ---------------------------------------------------------------------------
    def submit(self, xuid: int, views: list) -> int:
        config = load_config()
        written = 0
        now = time.time()
        with _db_lock:
            for v in views:
                view = int(v["view"])
                props = {int(p["id"]): p for p in v.get("props", [])}
                if not props:
                    continue
                self.conn.execute("INSERT INTO submissions (view,xuid,props,received) "
                                  "VALUES (?,?,?,?)", (view, xuid, json.dumps(v), now))
                cfg = config.get(view, {})
                default_id = TRIALS_RATING if TRIALS_RATING in props else next(iter(props))
                rating_id = int(cfg.get("rating", default_id))
                desc = cfg.get("order", "desc" if rating_id == TRIALS_RATING else "asc") == "desc"
                if rating_id not in props:
                    log.warning("view %d: rating property %d missing in %s", view, rating_id,
                                list(props))
                    continue
                rating = int(props[rating_id]["value"])
                old = self.conn.execute("SELECT rating, props FROM entries WHERE view=? AND xuid=?",
                                        (view, xuid)).fetchone()
                better = old is None or (rating > old["rating"] if desc else rating < old["rating"])
                if better:
                    merged = {} if old is None else {int(k): p for k, p in
                                                     json.loads(old["props"]).items()}
                    merged.update(props)
                    self.conn.execute(
                        "INSERT OR REPLACE INTO entries VALUES (?,?,?,?,?)",
                        (view, xuid, rating, json.dumps({str(k): p for k, p in merged.items()}),
                         now))
                    written += 1
                if view not in config:
                    log.info("view %d: rating %d (%s), %d props", view, rating,
                             "desc" if desc else "asc", len(props))
            self.conn.commit()
        return written

    # -- reading ---------------------------------------------------------------------------
    def _order(self, view: int) -> str:
        order = load_config().get(view, {}).get("order")
        if order is None:  # Trials HD views are ranked by TRIALS_RATING, higher first
            order = "desc"
        return "DESC" if order == "desc" else "ASC"

    def _rows(self, view: int, rows) -> list:
        names = self.names(r["xuid"] for r in rows)
        out = []
        for r in rows:
            out.append({
                "xuid": f"{r['xuid']:016X}",
                "name": names.get(r["xuid"], "?"),
                "rank": r["rank"],
                "rating": r["rating"],
                "props": json.loads(r["props"]),
            })
        return out

    def _ranked(self, view: int) -> str:
        o = self._order(view)
        return (f"SELECT xuid, rating, props, RANK() OVER (ORDER BY rating {o}) AS rank "
                f"FROM entries WHERE view={int(view)}")

    def total(self, view: int) -> int:
        return self.conn.execute("SELECT COUNT(*) FROM entries WHERE view=?", (view,)).fetchone()[0]

    def by_rank(self, view: int, start: int, count: int) -> list:
        rows = self.conn.execute(f"SELECT * FROM ({self._ranked(view)}) WHERE rank >= ? "
                                 f"ORDER BY rank LIMIT ?", (max(start, 1), count)).fetchall()
        return self._rows(view, rows)

    def around(self, view: int, xuid: int, count: int) -> list:
        me = self.conn.execute(f"SELECT rank FROM ({self._ranked(view)}) WHERE xuid=?",
                               (xuid,)).fetchone()
        if me is None:
            return self.by_rank(view, 1, count)
        start = max(1, me["rank"] - count // 2)
        return self.by_rank(view, start, count)

    def by_xuids(self, view: int, xuids: list) -> list:
        if not xuids:
            return []
        q = ",".join("?" * len(xuids))
        rows = self.conn.execute(f"SELECT * FROM ({self._ranked(view)}) WHERE xuid IN ({q}) "
                                 f"ORDER BY rank", xuids).fetchall()
        return self._rows(view, rows)

    # -- friends ---------------------------------------------------------------------------
    def friends(self, xuid: int) -> list:
        rows = self.conn.execute("SELECT friend FROM friends WHERE xuid=?", (xuid,)).fetchall()
        ids = [r["friend"] for r in rows]
        names = self.names(ids)
        return [{"xuid": f"{i:016X}", "name": names.get(i, "?")} for i in ids]

    def add_friend(self, xuid: int, name: str) -> None:
        other = xuid_for(name)
        if not self.names([other]):
            raise ApiError(404, f"no player called {name!r}")
        with _db_lock:
            self.conn.execute("INSERT OR IGNORE INTO friends VALUES (?,?)", (xuid, other))
            self.conn.execute("INSERT OR IGNORE INTO friends VALUES (?,?)", (other, xuid))
            self.conn.commit()


def board_text(board: dict) -> str:
    """Compact format for the game client: a 'total' line, then one tab-separated line per row:
    xuid, rank, rating, name, id:type:value;id:type:value..."""
    out = [f"total\t{board['total']}"]
    for r in board["rows"]:
        props = ";".join(f"{p['id']}:{p['type']}:{p['value']}" for p in r["props"].values())
        out.append(f"{r['xuid']}\t{r['rank']}\t{r['rating']}\t{r['name']}\t{props}")
    return "\n".join(out) + "\n"


class Handler(BaseHTTPRequestHandler):
    server_version = "TrialsHDServer/1"
    store: Store = None

    def log_message(self, fmt, *args):  # route through logging
        log.info("%s %s", self.address_string(), fmt % args)

    def _send(self, status: int, obj, text: bool = False) -> None:
        body = (board_text(obj) if text and status == 200 else json.dumps(obj)).encode()
        self.send_response(status)
        self.send_header("Content-Type", "text/plain" if text else "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        if n > MAX_BODY:
            raise ApiError(413, "request too large")
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except json.JSONDecodeError:
            raise ApiError(400, "invalid JSON")

    def _player(self) -> int:
        return self.store.authenticate(self.headers.get("X-Player-Name", ""),
                                       self.headers.get("X-Player-Key", ""))

    def _handle(self, method: str) -> None:
        with _db_lock:
            self._handle_locked(method)

    def _handle_locked(self, method: str) -> None:
        url = urlparse(self.path)
        q = {k: v[-1] for k, v in parse_qs(url.query).items()}
        try:
            if method == "GET" and url.path == "/health":
                return self._send(200, {"ok": True})
            if method == "GET" and url.path == "/api/v1/hello":
                xuid = self._player()
                return self._send(200, {"xuid": f"{xuid:016X}"})
            if method == "POST" and url.path == "/api/v1/stats":
                xuid = self._player()
                n = self.store.submit(xuid, self._body().get("views", []))
                return self._send(200, {"updated": n})
            if method == "GET" and url.path == "/api/v1/leaderboard":
                self._player()
                view = int(q["view"])
                count = min(int(q.get("count", 10)), 100)
                if "around" in q:
                    rows = self.store.around(view, int(q["around"], 16), count)
                else:
                    rows = self.store.by_rank(view, int(q.get("start", 1)), count)
                return self._send(200, {"view": view, "total": self.store.total(view),
                                        "rows": rows}, q.get("fmt") == "text")
            if method == "POST" and url.path == "/api/v1/read":
                self._player()
                body = self._body()
                view = int(body["view"])
                xuids = [int(x, 16) for x in body.get("xuids", [])][:100]
                return self._send(200, {"view": view, "total": self.store.total(view),
                                        "rows": self.store.by_xuids(view, xuids)},
                                  q.get("fmt") == "text")
            if method == "GET" and url.path == "/api/v1/friends":
                friends = self.store.friends(self._player())
                if q.get("fmt") == "text":
                    body = "".join(f"{f['xuid']}\t{f['name']}\n" for f in friends).encode()
                    self.send_response(200)
                    self.send_header("Content-Type", "text/plain")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                return self._send(200, {"friends": friends})
            if method == "POST" and url.path == "/api/v1/friends":
                xuid = self._player()
                self.store.add_friend(xuid, str(self._body().get("name", "")))
                return self._send(200, {"friends": self.store.friends(xuid)})
            if url.path.startswith("/api/v1/admin/"):
                if not ADMIN_KEY or not hmac.compare_digest(self.headers.get("X-Admin-Key", ""),
                                                            ADMIN_KEY):
                    raise ApiError(403, "admin key required")
                if method == "GET" and url.path == "/api/v1/admin/players":
                    rows = self.store.conn.execute("SELECT xuid, name, created FROM players")
                    return self._send(200, [{"xuid": f"{r['xuid']:016X}", "name": r["name"],
                                             "created": r["created"]} for r in rows])
                if method == "POST" and url.path == "/api/v1/admin/reset_key":
                    name = str(self._body().get("name", ""))
                    with _db_lock:
                        self.store.conn.execute("DELETE FROM players WHERE name=?", (name,))
                        self.store.conn.commit()
                    return self._send(200, {"reset": name})
            raise ApiError(404, "not found")
        except ApiError as e:
            self._send(e.status, {"error": str(e)})
        except (KeyError, ValueError) as e:
            self._send(400, {"error": f"bad request: {e}"})
        except Exception:
            log.exception("error handling %s %s", method, self.path)
            self._send(500, {"error": "internal error"})

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    os.makedirs(os.path.dirname(DB_PATH) or ".", exist_ok=True)
    conn = db()
    init_db(conn)
    Handler.store = Store(conn)
    log.info("Trials HD leaderboard server on port %d, database %s", PORT, DB_PATH)
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()


if __name__ == "__main__":
    main()
