import asyncio
import os
import json
import time
import uuid
import argparse
import subprocess
from urllib.parse import urlparse, parse_qs

from websockets.legacy.server import serve

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HTML = os.path.join(HERE, "sensor.html")
DEFAULT_LOG = os.path.join(HERE, "gyro_log.ndjson")
DEFAULT_LATEST = os.path.join(HERE, "latest.json")


# -------------------------
# Globals
# -------------------------
clients_by_ws = {}   # ws -> info dict
ws_by_uid = {}       # uid -> ws (latest connection)
latest_by_uid = {}   # uid -> last payload dict

HTML_PATH = DEFAULT_HTML
LOG_PATH = DEFAULT_LOG
LATEST_PATH = DEFAULT_LATEST


# -------------------------
# Utils
# -------------------------
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

def json_loads_safe(raw: str):
    try:
        obj = json.loads(raw)
        return obj if isinstance(obj, dict) else {"type": "raw", "raw": obj}
    except Exception:
        return {"type": "raw", "raw": raw}

async def send_json(ws, obj) -> bool:
    try:
        await ws.send(json_dumps(obj))
        return True
    except Exception:
        return False

async def broadcast_to_role(role: str, obj, exclude_ws=None) -> int:
    sent = 0
    dead = []
    for w, info in list(clients_by_ws.items()):
        if exclude_ws is not None and w == exclude_ws:
            continue
        if info.get("role") == role:
            ok = await send_json(w, obj)
            if ok:
                sent += 1
            else:
                dead.append(w)
    for w in dead:
        await drop_client(w)
    return sent

def read_file_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()

def http_response(code: int, body: bytes, content_type: str):
    headers = [
        ("Content-Type", content_type),
        ("Content-Length", str(len(body))),
        ("Cache-Control", "no-store"),
    ]
    return (code, headers, body)

def launch_tail_window(file_path: str):
    try:
        cmd = ["powershell", "-NoExit", "-Command", f'Get-Content "{file_path}" -Wait']
        subprocess.Popen(cmd, creationflags=subprocess.CREATE_NEW_CONSOLE)
        print("[TAIL] launched PowerShell tail window")
    except Exception as e:
        print("[TAIL] failed:", repr(e))


# -------------------------
# Client lifecycle
# -------------------------
async def drop_client(ws):
    info = clients_by_ws.get(ws)
    if not info:
        return

    uid = info.get("uid", "")
    role = info.get("role", "")
    name = info.get("name", "")

    # uid mapping cleanup
    if uid and ws_by_uid.get(uid) == ws:
        ws_by_uid.pop(uid, None)

    clients_by_ws.pop(ws, None)

    # notify UE when phone disconnects (optional but handy)
    if role == "phone":
        await broadcast_to_role("ue", {
            "type": "device_disconnected",
            "server_ts": now(),
            "uid": uid,
            "name": name,
            "remote": info.get("remote", "")
        })

    try:
        await ws.close()
    except Exception:
        pass


# -------------------------
# WS handler
# -------------------------
async def ws_handler(ws, path):
    p, qs = parse_path(path)
    if p.rstrip("/") != "/ws":
        await ws.close()
        return

    # role fixed at connect time
    role_q = (qs.get("role", ["phone"])[0] or "phone").strip().lower()
    if role_q not in ("phone", "ue"):
        role_q = "phone"

    uid_q = safe_id(qs.get("uid", [""])[0], fallback=str(uuid.uuid4()))
    name_q = (qs.get("name", [""])[0] or "").strip()[:64]

    try:
        remote = f"{ws.remote_address[0]}:{ws.remote_address[1]}"
    except Exception:
        remote = str(ws.remote_address)

    info = {
        "uid": uid_q,
        "name": name_q,
        "role": role_q,
        "remote": remote,
        "connected_at": now(),
        "last_seen": now(),
        "recv_count": 0,
    }

    clients_by_ws[ws] = info
    ws_by_uid[uid_q] = ws

    print(f"[WS] connected uid={uid_q} name={name_q} role={role_q} remote={remote} clients={len(clients_by_ws)}")

    # hello ack
    await send_json(ws, {
        "type": "server_hello",
        "server_ts": now(),
        "uid": uid_q,
        "name": name_q,
        "role": role_q
    })

    # phone connect -> notify UE (optional)
    if role_q == "phone":
        await broadcast_to_role("ue", {
            "type": "device_connected",
            "server_ts": now(),
            "uid": uid_q,
            "name": name_q,
            "remote": remote
        })

    # UE connect -> send phone list once (optional)
    if role_q == "ue":
        devices = []
        for ci in clients_by_ws.values():
            if ci.get("role") == "phone":
                devices.append({
                    "uid": ci.get("uid", ""),
                    "name": ci.get("name", ""),
                    "remote": ci.get("remote", ""),
                    "last_seen": ci.get("last_seen", 0),
                })
        await send_json(ws, {
            "type": "device_list",
            "server_ts": now(),
            "devices": devices
        })

    try:
        async for raw in ws:
            info["recv_count"] += 1
            info["last_seen"] = now()

            if isinstance(raw, (bytes, bytearray)):
                raw = raw.decode("utf-8", errors="ignore")

            obj = json_loads_safe(raw)
            typ = (obj.get("type") or "").strip()

            # normalize uid/name (role remains fixed)
            obj_uid = safe_id(obj.get("uid") or info["uid"], info["uid"])
            obj_name = (obj.get("name") or info["name"] or "")[:64]

            # if uid changed mid-stream, update mapping
            if obj_uid != info["uid"]:
                if ws_by_uid.get(info["uid"]) == ws:
                    ws_by_uid.pop(info["uid"], None)
                info["uid"] = obj_uid
                ws_by_uid[obj_uid] = ws

            info["name"] = obj_name

            # only accept IMU from phone
            if typ == "imu":
                if info["role"] != "phone":
                    await send_json(ws, {"type": "error", "msg": "imu only accepted from role=phone"})
                    continue

                # Keep only sensor-related fields + ensure Fire exists
                payload = {
                    "type": "imu",
                    "server_ts": now(),
                    "uid": info["uid"],
                    "name": info["name"],
                    "role": "phone",

                    # ---- original sensor fields (pass-through if present) ----
                    "ts": obj.get("ts", obj.get("TsMs", 0)),
                    "match_id": obj.get("match_id", ""),

                    "yaw": obj.get("yaw", obj.get("Yaw", 0)),
                    "pitch": obj.get("pitch", obj.get("Pitch", 0)),
                    "roll": obj.get("roll", obj.get("Roll", 0)),

                    "ax": obj.get("ax", obj.get("Ax", 0)),
                    "ay": obj.get("ay", obj.get("Ay", 0)),
                    "az": obj.get("az", obj.get("Az", 0)),

                    "gx": obj.get("gx", obj.get("Gx", 0)),
                    "gy": obj.get("gy", obj.get("Gy", 0)),
                    "gz": obj.get("gz", obj.get("Gz", 0)),

                    # ---- Fire ----
                    "fire": int(obj.get("fire", obj.get("Fire", 0)) or 0),
                }

                # remember latest
                latest_by_uid[info["uid"]] = payload
                try:
                    with open(LATEST_PATH, "w", encoding="utf-8") as f:
                        json.dump(payload, f, ensure_ascii=False, indent=2)
                except Exception:
                    pass

                # ndjson log (optional)
                try:
                    with open(LOG_PATH, "a", encoding="utf-8") as f:
                        f.write(json.dumps(payload, ensure_ascii=False) + "\n")
                        f.flush()
                except Exception as e:
                    print("[LOG] write error:", repr(e))

                # broadcast to UE listeners
                sent = await broadcast_to_role("ue", payload, exclude_ws=None)
                if sent == 0 and (info["recv_count"] % 120 == 0):
                    print("[WARN] no UE clients connected (imu received but nobody to forward)")

                continue

            # allow simple ping
            if typ == "hello":
                await send_json(ws, {"type": "hello_ack", "server_ts": now(), "uid": info["uid"]})
                continue

            # ignore everything else (keeps it clean)
            # await send_json(ws, {"type":"ignored","msg":"unknown type"})
            continue

    except Exception as e:
        print("[WS] handler error:", repr(e))
    finally:
        await drop_client(ws)
        print(f"[WS] disconnected remote={remote} clients={len(clients_by_ws)}")


# -------------------------
# HTTP endpoints (minimal)
# -------------------------
def process_request(path, request_headers):
    p, qs = parse_path(path)
    p2 = p.rstrip("/") if p != "/" else "/"

    # upgrade path
    if p2 == "/ws":
        return None

    # serve sensor html
    if p2 == "/" or p2 == "/sensor.html":
        if not os.path.exists(HTML_PATH):
            body = f"sensor.html not found: {HTML_PATH}".encode("utf-8")
            return http_response(404, body, "text/plain; charset=utf-8")
        body = read_file_bytes(HTML_PATH)
        return http_response(200, body, "text/html; charset=utf-8")

    if p2 == "/stats":
            clients = []
            for ci in clients_by_ws.values():
                clients.append({
                    "uid": ci.get("uid", ""),
                    "name": ci.get("name", ""),
                    "role": ci.get("role", ""),
                    "remote": ci.get("remote", ""),
                    "connected_at": ci.get("connected_at", 0),
                    "last_seen": ci.get("last_seen", 0),
                    "recv_count": ci.get("recv_count", 0),
                })
            obj = {"type": "stats", "server_ts": now(), "clients": clients}
            return http_response(
                200,
                json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                "application/json; charset=utf-8"
            )

    # optional debug: latest by uid  (/latest?uid=xxx)
    if p2 == "/latest":
        uid = (qs.get("uid", [""])[0] or "").strip()
        obj = latest_by_uid.get(uid) if uid else {}
        return http_response(
            200,
            json.dumps(obj or {}, ensure_ascii=False, indent=2).encode("utf-8"),
            "application/json; charset=utf-8"
        )
    

    return http_response(404, b"Not Found", "text/plain; charset=utf-8")


# -------------------------
# Main
# -------------------------
async def main():
    global HTML_PATH, LOG_PATH, LATEST_PATH

    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", default=8080, type=int)
    ap.add_argument("--html", default=DEFAULT_HTML)
    ap.add_argument("--log", default=DEFAULT_LOG)
    ap.add_argument("--latest", default=DEFAULT_LATEST)
    ap.add_argument("--tail", action="store_true")
    args = ap.parse_args()

    HTML_PATH = args.html if os.path.isabs(args.html) else os.path.join(HERE, args.html)
    LOG_PATH = args.log if os.path.isabs(args.log) else os.path.join(HERE, args.log)
    LATEST_PATH = args.latest if os.path.isabs(args.latest) else os.path.join(HERE, args.latest)

    if args.tail:
        launch_tail_window(LOG_PATH)

    print(f"HTTP+WS server: http://{args.host}:{args.port}")
    print("  UI     : /  (sensor.html)")
    print("  WS     : /ws?role=phone|ue&uid=...&name=...")
    print("  latest : /latest?uid=UID (debug)")
    print("HTML_PATH   =", HTML_PATH)
    print("LOG_PATH    =", LOG_PATH)
    print("LATEST_PATH =", LATEST_PATH)

    async with serve(
        ws_handler, args.host, args.port,
        process_request=process_request,
        ping_interval=20, ping_timeout=20,
        max_size=2**20
    ):
        await asyncio.Future()  # run forever

if __name__ == "__main__":
    asyncio.run(main())
