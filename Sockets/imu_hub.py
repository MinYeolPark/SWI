import asyncio
import os
import json
import time
import uuid
import argparse
import sqlite3
import subprocess
from dataclasses import dataclass, asdict
from urllib.parse import urlparse, parse_qs

from websockets.legacy.server import serve

# =========================
# Paths / Defaults
# =========================
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HTML   = os.path.join(HERE, "sensor.html")
DEFAULT_LOG    = os.path.join(HERE, "gyro_log.ndjson")
DEFAULT_LATEST = os.path.join(HERE, "latest.json")

# =========================
# Data Models
# =========================
@dataclass
class ClientInfo:
    uid: str
    name: str
    role: str  # "phone" or "ue"  (ROLE IS FIXED AT CONNECT TIME)
    remote: str
    connected_at: float
    last_seen: float
    recv_count: int = 0
    match_id: str = ""
    in_queue: bool = False

@dataclass
class MatchInfo:
    match_id: str
    created_ts: float
    started_ts: float
    ended_ts: float
    state: str  # "waiting"|"running"|"ended"|"aborted"
    p1_uid: str
    p1_name: str
    p2_uid: str
    p2_name: str
    winner_uid: str = ""
    result_json: str = ""

# =========================
# Globals
# =========================
clients_by_ws: dict = {}            # ws -> ClientInfo
ws_by_uid: dict[str, object] = {}   # uid -> ws (latest connection)
waiting_queue: list[str] = []       # list of phone uids
matches: dict[str, MatchInfo] = {}  # match_id -> MatchInfo

latest_by_uid: dict[str, dict] = {} # uid -> last json payload
recv_total = 0

LOG_PATH = DEFAULT_LOG
LATEST_PATH = DEFAULT_LATEST
HTML_PATH = DEFAULT_HTML

db_conn: sqlite3.Connection | None = None

# =========================
# Utils
# =========================
def now() -> float:
    return time.time()

def safe_id(s: str, fallback: str) -> str:
    s = (s or "").strip()
    if not s:
        return fallback
    s = "".join(ch for ch in s if ch.isalnum() or ch in "-_")
    return s[:64] if s else fallback

def parse_path(path: str):
    u = urlparse(path)
    qs = parse_qs(u.query)
    return u.path, qs

def json_dumps(obj) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))

async def send_json(ws, obj) -> bool:
    try:
        await ws.send(json_dumps(obj))
        return True
    except Exception:
        return False

async def broadcast_to_role(role: str, obj, exclude_ws=None):
    sent = 0
    dead = []
    for w, info in list(clients_by_ws.items()):
        if exclude_ws is not None and w == exclude_ws:
            continue
        if info.role == role:
            ok = await send_json(w, obj)
            if ok: sent += 1
            else: dead.append(w)
    print(f"[BCAST] role={role} type={obj.get('type')} sent={sent}")
    for w in dead:
        await drop_client(w)

async def broadcast_all(obj, exclude_ws=None):
    dead = []
    for w in list(clients_by_ws.keys()):
        if exclude_ws is not None and w == exclude_ws:
            continue
        ok = await send_json(w, obj)
        if not ok:
            dead.append(w)
    for w in dead:
        await drop_client(w)

async def send_to_uid(uid: str, obj) -> bool:
    w = ws_by_uid.get(uid)
    if not w:
        return False
    return await send_json(w, obj)

def remove_from_queue(uid: str):
    global waiting_queue
    if uid in waiting_queue:
        waiting_queue = [x for x in waiting_queue if x != uid]

def read_file_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()

def launch_tail_window(file_path: str):
    try:
        cmd = ["powershell", "-NoExit", "-Command", f'Get-Content "{file_path}" -Wait']
        subprocess.Popen(cmd, creationflags=subprocess.CREATE_NEW_CONSOLE)
        print("[TAIL] launched PowerShell tail window")
    except Exception as e:
        print("[TAIL] failed:", repr(e))

def http_response(code: int, body: bytes, content_type: str):
    headers = [
        ("Content-Type", content_type),
        ("Content-Length", str(len(body))),
        ("Cache-Control", "no-store"),
    ]
    return (code, headers, body)

def json_loads_safe(raw: str):
    try:
        obj = json.loads(raw)
        if isinstance(obj, dict):
            return obj
        return {"type": "raw", "raw": obj}
    except Exception:
        return {"type": "raw", "raw": raw}

# =========================
# DB
# =========================
def db_init(db_path: str):
    global db_conn
    db_conn = sqlite3.connect(db_path, isolation_level=None)  # autocommit
    db_conn.execute("PRAGMA journal_mode=WAL;")
    db_conn.execute("PRAGMA synchronous=NORMAL;")

    db_conn.execute("""
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            server_ts REAL NOT NULL,
            uid TEXT NOT NULL,
            name TEXT,
            role TEXT,
            type TEXT,
            match_id TEXT,
            payload_json TEXT NOT NULL
        )
    """)
    db_conn.execute("CREATE INDEX IF NOT EXISTS idx_events_uid_ts ON events(uid, server_ts)")
    db_conn.execute("CREATE INDEX IF NOT EXISTS idx_events_type_ts ON events(type, server_ts)")
    db_conn.execute("CREATE INDEX IF NOT EXISTS idx_events_match_ts ON events(match_id, server_ts)")

    db_conn.execute("""
        CREATE TABLE IF NOT EXISTS matches (
            match_id TEXT PRIMARY KEY,
            created_ts REAL NOT NULL,
            started_ts REAL NOT NULL,
            ended_ts REAL NOT NULL,
            state TEXT NOT NULL,
            p1_uid TEXT NOT NULL,
            p1_name TEXT,
            p2_uid TEXT NOT NULL,
            p2_name TEXT,
            winner_uid TEXT,
            result_json TEXT
        )
    """)
    db_conn.execute("CREATE INDEX IF NOT EXISTS idx_matches_state_ts ON matches(state, started_ts)")

    db_conn.execute("""
        CREATE TABLE IF NOT EXISTS player_stats (
            uid TEXT PRIMARY KEY,
            name TEXT,
            games_played INTEGER NOT NULL DEFAULT 0,
            wins INTEGER NOT NULL DEFAULT 0,
            last_seen REAL NOT NULL DEFAULT 0
        )
    """)
    db_conn.execute("CREATE INDEX IF NOT EXISTS idx_stats_wins ON player_stats(wins)")

def db_insert_event(server_ts: float, uid: str, name: str, role: str, typ: str, match_id: str, payload_obj: dict):
    if not db_conn:
        return
    db_conn.execute(
        "INSERT INTO events(server_ts, uid, name, role, type, match_id, payload_json) VALUES (?,?,?,?,?,?,?)",
        (server_ts, uid, name, role, typ, match_id, json.dumps(payload_obj, ensure_ascii=False))
    )

def db_upsert_match(m: MatchInfo):
    if not db_conn:
        return
    db_conn.execute("""
        INSERT INTO matches(match_id, created_ts, started_ts, ended_ts, state, p1_uid, p1_name, p2_uid, p2_name, winner_uid, result_json)
        VALUES (?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(match_id) DO UPDATE SET
            ended_ts=excluded.ended_ts,
            state=excluded.state,
            winner_uid=excluded.winner_uid,
            result_json=excluded.result_json
    """, (
        m.match_id, m.created_ts, m.started_ts, m.ended_ts, m.state,
        m.p1_uid, m.p1_name, m.p2_uid, m.p2_name, m.winner_uid, m.result_json
    ))

def db_update_stats_on_result(puid: str, pname: str, won: bool, ts: float):
    if not db_conn:
        return
    row = db_conn.execute("SELECT games_played, wins FROM player_stats WHERE uid=?", (puid,)).fetchone()
    if row is None:
        games = 1
        wins = 1 if won else 0
        db_conn.execute(
            "INSERT INTO player_stats(uid, name, games_played, wins, last_seen) VALUES (?,?,?,?,?)",
            (puid, pname, games, wins, ts)
        )
    else:
        games, wins = row[0], row[1]
        games += 1
        if won:
            wins += 1
        db_conn.execute(
            "UPDATE player_stats SET name=?, games_played=?, wins=?, last_seen=? WHERE uid=?",
            (pname, games, wins, ts, puid)
        )

def db_get_leaderboard(limit: int = 20):
    if not db_conn:
        return []
    limit = max(1, min(limit, 200))
    cur = db_conn.execute("""
        SELECT uid, name, games_played, wins, last_seen
        FROM player_stats
        ORDER BY wins DESC, games_played DESC, last_seen DESC
        LIMIT ?
    """, (limit,))
    out = []
    for r in cur.fetchall():
        out.append({"uid": r[0], "name": r[1], "games": r[2], "wins": r[3], "last_seen": r[4]})
    return out

def db_get_recent_matches(limit: int = 20):
    if not db_conn:
        return []
    limit = max(1, min(limit, 200))
    cur = db_conn.execute("""
        SELECT match_id, started_ts, ended_ts, state, p1_uid, p1_name, p2_uid, p2_name, winner_uid
        FROM matches
        ORDER BY started_ts DESC
        LIMIT ?
    """, (limit,))
    out = []
    for r in cur.fetchall():
        out.append({
            "match_id": r[0],
            "started_ts": r[1],
            "ended_ts": r[2],
            "state": r[3],
            "p1": {"uid": r[4], "name": r[5]},
            "p2": {"uid": r[6], "name": r[7]},
            "winner_uid": r[8],
        })
    return out

# =========================
# Matchmaking
# =========================
def make_match_id() -> str:
    return f"m{int(time.time()*1000)}_{uuid.uuid4().hex[:6]}"

def get_client_by_uid(uid: str) -> ClientInfo | None:
    w = ws_by_uid.get(uid)
    if not w:
        return None
    return clients_by_ws.get(w)

async def try_start_matches():
    """If >=2 phones in queue, create match and notify both phones + all UE."""
    while len(waiting_queue) >= 2:
        uid1 = waiting_queue.pop(0)
        uid2 = waiting_queue.pop(0)

        c1 = get_client_by_uid(uid1)
        c2 = get_client_by_uid(uid2)
        if not c1 or not c2:
            continue

        match_id = make_match_id()
        ts = now()
        m = MatchInfo(
            match_id=match_id,
            created_ts=ts,
            started_ts=ts,
            ended_ts=0.0,
            state="running",
            p1_uid=c1.uid, p1_name=c1.name,
            p2_uid=c2.uid, p2_name=c2.name
        )
        matches[match_id] = m

        c1.match_id = match_id
        c2.match_id = match_id
        c1.in_queue = False
        c2.in_queue = False

        if db_conn:
            db_upsert_match(m)

        payload = {
            "type": "match_start",
            "server_ts": ts,
            "match_id": match_id,
            "players": [
                {"uid": c1.uid, "name": c1.name},
                {"uid": c2.uid, "name": c2.name},
            ]
        }

        await send_to_uid(c1.uid, payload)
        await send_to_uid(c2.uid, payload)
        await broadcast_to_role("ue", payload)

        print(f"[MATCH] start {match_id} ({c1.uid},{c2.uid})")

async def abort_match(match_id: str, reason: str):
    m = matches.get(match_id)
    if not m:
        return
    if m.state in ("ended", "aborted"):
        return
    m.state = "aborted"
    m.ended_ts = now()
    m.result_json = json.dumps({"reason": reason}, ensure_ascii=False)
    if db_conn:
        db_upsert_match(m)

    payload = {
        "type": "match_abort",
        "server_ts": now(),
        "match_id": match_id,
        "reason": reason
    }
    await send_to_uid(m.p1_uid, payload)
    await send_to_uid(m.p2_uid, payload)
    await broadcast_to_role("ue", payload)

# =========================
# Client lifecycle
# =========================
async def drop_client(ws):
    info = clients_by_ws.get(ws)
    if not info:
        return

    # snapshot
    snap = {
        "uid": info.uid,
        "name": info.name,
        "role": info.role,
        "remote": info.remote,
        "match_id": info.match_id
    }

    # remove mapping
    if ws_by_uid.get(info.uid) == ws:
        ws_by_uid.pop(info.uid, None)

    # remove from queue
    remove_from_queue(info.uid)

    # if in running match -> abort
    if info.match_id:
        mid = info.match_id
        m = matches.get(mid)
        if m and m.state == "running":
            await abort_match(mid, reason=f"disconnect:{info.uid}")

    clients_by_ws.pop(ws, None)

    # notify UE about phone disconnect
    if info.role == "phone":
        await broadcast_to_role("ue", {
            "type": "device_disconnected",
            "server_ts": now(),
            "uid": info.uid,
            "name": info.name,
            "role": info.role,
            "remote": info.remote
        })

    try:
        await ws.close()
    except Exception:
        pass

# =========================
# WS handler
# =========================
async def ws_handler(ws, path):
    global recv_total

    p, qs = parse_path(path)
    if p.rstrip("/") != "/ws":
        await ws.close()
        return

    # role fixed at connect time (can't be changed by payload)
    role_q = (qs.get("role", ["phone"])[0] or "phone").strip().lower()
    if role_q not in ("phone", "ue"):
        role_q = "phone"

    uid_q = safe_id(qs.get("uid", [""])[0], fallback=str(uuid.uuid4()))
    name_q = (qs.get("name", [""])[0] or "").strip()[:64]

    try:
        remote = f"{ws.remote_address[0]}:{ws.remote_address[1]}"
    except Exception:
        remote = str(ws.remote_address)

    info = ClientInfo(
        uid=uid_q, name=name_q, role=role_q,
        remote=remote, connected_at=now(), last_seen=now()
    )
    clients_by_ws[ws] = info
    ws_by_uid[info.uid] = ws

    print(f"[WS] connected uid={info.uid} name={info.name} role={info.role} remote={remote} clients={len(clients_by_ws)}")

    # hello ack
    await send_json(ws, {
        "type": "server_hello",
        "server_ts": now(),
        "uid": info.uid,
        "name": info.name,
        "role": info.role
    })

    # NEW: phone connect -> notify UE
    if info.role == "phone":
        await broadcast_to_role("ue", {
            "type": "device_connected",
            "server_ts": now(),
            "uid": info.uid,
            "name": info.name,
            "role": info.role,
            "remote": info.remote
        })

    # NEW: UE connect -> send current phone list once
    if info.role == "ue":
        devices = []
        for ci in clients_by_ws.values():
            if ci.role == "phone":
                devices.append({
                    "uid": ci.uid,
                    "name": ci.name,
                    "remote": ci.remote,
                    "connected_at": ci.connected_at,
                    "last_seen": ci.last_seen,
                })
        await send_json(ws, {
            "type": "device_list",
            "server_ts": now(),
            "devices": devices
        })

    try:
        async for raw in ws:
            recv_total += 1
            info.recv_count += 1
            info.last_seen = now()

            if isinstance(raw, (bytes, bytearray)):
                raw = raw.decode("utf-8", errors="ignore")

            obj = json_loads_safe(raw)

            # normalize uid/name only (role fixed)
            typ = (obj.get("type") or "").strip()
            obj_uid = safe_id(obj.get("uid") or info.uid, info.uid)
            obj_name = (obj.get("name") or info.name or "")[:64]

            # if uid changed, update mapping
            if obj_uid != info.uid:
                if ws_by_uid.get(info.uid) == ws:
                    ws_by_uid.pop(info.uid, None)
                info.uid = obj_uid
                ws_by_uid[info.uid] = ws
            info.name = obj_name

            # match_id in payload or inferred
            msg_match_id = (obj.get("match_id") or info.match_id or "").strip()

            # remember latest
            latest_by_uid[info.uid] = obj
            try:
                with open(LATEST_PATH, "w", encoding="utf-8") as f:
                    json.dump(obj, f, ensure_ascii=False, indent=2)
            except Exception:
                pass

            # ndjson log
            line = {"server_ts": now(), "uid": info.uid, "name": info.name, "role": info.role, "payload": obj}
            try:
                with open(LOG_PATH, "a", encoding="utf-8") as f:
                    f.write(json.dumps(line, ensure_ascii=False) + "\n")
                    f.flush()
            except Exception as e:
                print("[LOG] write error:", repr(e))

            # DB event
            if db_conn:
                try:
                    db_insert_event(line["server_ts"], info.uid, info.name, info.role, typ, msg_match_id, obj)
                except Exception as e:
                    print("[DB] event insert error:", repr(e))

            # ============ Protocol handling ============
            if typ == "hello":
                await send_json(ws, {"type": "hello_ack", "server_ts": now(), "uid": info.uid})
                continue

            if typ == "join_request":
                if info.role != "phone":
                    await send_json(ws, {"type": "error", "msg": "join_request only for phone"})
                    continue
                if info.match_id:
                    await send_json(ws, {"type": "already_in_match", "match_id": info.match_id})
                    continue

                if info.uid not in waiting_queue:
                    waiting_queue.append(info.uid)
                    info.in_queue = True

                await send_json(ws, {
                    "type": "queue_status",
                    "server_ts": now(),
                    "queued": True,
                    "queue_len": len(waiting_queue)
                })

                await try_start_matches()
                continue

            if typ in ("leave_queue", "leave"):
                remove_from_queue(info.uid)
                info.in_queue = False

                if info.match_id:
                    mid = info.match_id
                    info.match_id = ""
                    await abort_match(mid, reason=f"leave:{info.uid}")

                await send_json(ws, {"type": "left", "server_ts": now()})
                continue

            if typ == "imu":
                if info.match_id:
                    obj["match_id"] = info.match_id

                # broadcast to all UE listeners
                await broadcast_to_role("ue", {"type": "imu", **obj}, exclude_ws=None)

                # optional: send to opponent phone
                if info.match_id:
                    m = matches.get(info.match_id)
                    if m and m.state == "running":
                        opp = m.p2_uid if info.uid == m.p1_uid else m.p1_uid
                        await send_to_uid(opp, {"type": "opponent_imu", **obj})

                continue

            if typ == "chat":
                if info.match_id:
                    m = matches.get(info.match_id)
                    if m:
                        await send_to_uid(m.p1_uid, {"type": "chat", **obj})
                        await send_to_uid(m.p2_uid, {"type": "chat", **obj})
                        await broadcast_to_role("ue", {"type": "chat", **obj})
                continue

            if typ == "match_result":
                if info.role != "ue":
                    await send_json(ws, {"type": "error", "msg": "match_result only for ue"})
                    continue

                match_id = (obj.get("match_id") or "").strip()
                if not match_id or match_id not in matches:
                    await send_json(ws, {"type": "error", "msg": "unknown match_id"})
                    continue

                m = matches[match_id]
                if m.state != "running":
                    await send_json(ws, {"type": "error", "msg": f"match not running (state={m.state})"})
                    continue

                m.state = "ended"
                m.ended_ts = now()
                m.winner_uid = (obj.get("winner_uid") or "").strip()
                m.result_json = json.dumps(obj, ensure_ascii=False)

                if db_conn:
                    db_upsert_match(m)
                    p1_won = (m.winner_uid == m.p1_uid)
                    p2_won = (m.winner_uid == m.p2_uid)
                    db_update_stats_on_result(m.p1_uid, m.p1_name, p1_won, m.ended_ts)
                    db_update_stats_on_result(m.p2_uid, m.p2_name, p2_won, m.ended_ts)

                c1 = get_client_by_uid(m.p1_uid)
                c2 = get_client_by_uid(m.p2_uid)
                if c1: c1.match_id = ""
                if c2: c2.match_id = ""

                payload = {
                    "type": "match_end",
                    "server_ts": now(),
                    "match_id": match_id,
                    "winner_uid": m.winner_uid,
                    "result": obj
                }

                await send_to_uid(m.p1_uid, payload)
                await send_to_uid(m.p2_uid, payload)
                await broadcast_to_role("ue", payload)

                print(f"[MATCH] end {match_id} winner={m.winner_uid}")
                continue

            # default relay
            if info.role == "phone":
                await broadcast_to_role("ue", {"type": "relay", **obj})
            else:
                if msg_match_id and msg_match_id in matches:
                    mm = matches[msg_match_id]
                    await send_to_uid(mm.p1_uid, {"type": "relay", **obj})
                    await send_to_uid(mm.p2_uid, {"type": "relay", **obj})
                else:
                    await broadcast_to_role("phone", {"type": "relay", **obj})

            if recv_total % 200 == 0:
                print(f"[recv_total {recv_total}] clients={len(clients_by_ws)} queue={len(waiting_queue)} matches={len(matches)}")

    except Exception as e:
        print("[WS] handler error:", repr(e))
    finally:
        await drop_client(ws)
        print(f"[WS] disconnected remote={remote} clients={len(clients_by_ws)}")

# =========================
# HTTP endpoints
# =========================
def process_request(path, request_headers):
    p, qs = parse_path(path)
    p2 = p.rstrip("/") if p != "/" else "/"

    # upgrade path
    if p2 == "/ws":
        return None

    # UI html
    if p2 == "/" or p2 == "/sensor.html":
        if not os.path.exists(HTML_PATH):
            body = f"sensor.html not found: {HTML_PATH}".encode("utf-8")
            return http_response(404, body, "text/plain; charset=utf-8")
        body = read_file_bytes(HTML_PATH)
        return http_response(200, body, "text/html; charset=utf-8")

    if p2 == "/stats":
        cl = [asdict(info) for info in clients_by_ws.values()]
        payload = {
            "server_ts": now(),
            "clients": cl,
            "clients_count": len(cl),
            "queue": waiting_queue,
            "queue_len": len(waiting_queue),
            "matches_running": [m.match_id for m in matches.values() if m.state == "running"],
            "matches_count": len(matches),
            "db_enabled": bool(db_conn),
            "log_path": LOG_PATH,
            "latest_path": LATEST_PATH,
            "html_path": HTML_PATH
        }
        return http_response(200, json.dumps(payload, ensure_ascii=False, indent=2).encode("utf-8"),
                             "application/json; charset=utf-8")

    if p2 == "/leaderboard":
        if not db_conn:
            return http_response(503, b"DB not enabled. Run with --db imu.db", "text/plain; charset=utf-8")
        n = int((qs.get("n", ["20"])[0] or "20"))
        rows = db_get_leaderboard(n)
        return http_response(200, json.dumps({"rows": rows}, ensure_ascii=False, indent=2).encode("utf-8"),
                             "application/json; charset=utf-8")

    if p2 == "/matches":
        if not db_conn:
            return http_response(503, b"DB not enabled. Run with --db imu.db", "text/plain; charset=utf-8")
        n = int((qs.get("n", ["20"])[0] or "20"))
        rows = db_get_recent_matches(n)
        return http_response(200, json.dumps({"rows": rows}, ensure_ascii=False, indent=2).encode("utf-8"),
                             "application/json; charset=utf-8")

    if p2 == "/latest":
        uid = (qs.get("uid", [""])[0] or "").strip()
        obj = latest_by_uid.get(uid) if uid else None
        return http_response(200, json.dumps(obj or {}, ensure_ascii=False, indent=2).encode("utf-8"),
                             "application/json; charset=utf-8")

    return http_response(404, b"Not Found", "text/plain; charset=utf-8")

# =========================
# Main
# =========================
async def main():
    global LOG_PATH, LATEST_PATH, HTML_PATH

    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", default=8080, type=int)
    ap.add_argument("--html", default=DEFAULT_HTML)
    ap.add_argument("--log", default=DEFAULT_LOG)
    ap.add_argument("--latest", default=DEFAULT_LATEST)
    ap.add_argument("--tail", action="store_true")
    ap.add_argument("--db", default="")
    args = ap.parse_args()

    HTML_PATH = args.html if os.path.isabs(args.html) else os.path.join(HERE, args.html)
    LOG_PATH = args.log if os.path.isabs(args.log) else os.path.join(HERE, args.log)
    LATEST_PATH = args.latest if os.path.isabs(args.latest) else os.path.join(HERE, args.latest)

    if args.db:
        db_path = args.db if os.path.isabs(args.db) else os.path.join(HERE, args.db)
        db_init(db_path)
        print("[DB] enabled:", db_path)

    if args.tail:
        launch_tail_window(LOG_PATH)

    print(f"HTTP+WS server: http://{args.host}:{args.port}")
    print("  UI         : /  (sensor.html)")
    print("  WS         : /ws")
    print("  stats      : /stats")
    print("  leaderboard: /leaderboard?n=20   (requires --db)")
    print("  matches    : /matches?n=20       (requires --db)")
    print("  latest     : /latest?uid=UID")
    print("HTML_PATH    =", HTML_PATH)
    print("LOG_PATH     =", LOG_PATH)
    print("LATEST_PATH  =", LATEST_PATH)

    async with serve(
        ws_handler, args.host, args.port,
        process_request=process_request,
        ping_interval=20, ping_timeout=20,
        max_size=2**20
    ):
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
