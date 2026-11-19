#!/usr/bin/env python3
"""A small stand-in for a Xenia Web Services server, for testing only.

It answers the REST calls the recomp's Xbox Live replacement makes
(thirdparty/rexglue-sdk/src/kernel/xam/live/live_api.cpp) the way the real
server does (https://github.com/AdrianCassar/Xenia-WebServices, NestJS):
same paths, same status codes (POST answers 201, errors are
{"message", "error", "statusCode"}), same checks on XUIDs, MAC addresses
and session IDs. Data is kept in memory only.

Session search follows the real server for games without XLast data: only
sessions with properties and contexts are listed, and when filters are sent
the session's GAME_MODE and GAME_TYPE contexts must match the filters'.

Usage:  python3 mock_server.py [--port 36000] [--host 127.0.0.1]
Used by: optroy_netcheck selftest http://127.0.0.1:36000
"""

import argparse
import base64
import json
import struct
import re
import secrets
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HEX16 = re.compile(r"^[0-9A-Fa-f]{16}$")
HEX12 = re.compile(r"^[0-9A-Fa-f]{12}$")

# Session flags (Xenia-WebServices src/domain/value-objects/SessionFlags.ts)
HOST = 1 << 0
PRESENCE = 1 << 1
STATS = 1 << 2
MATCHMAKING = 1 << 3
SINGLEPLAYER_WITH_STATS = PRESENCE | STATS | (1 << 8) | (1 << 9) | (1 << 10)


GAME_TYPE = 0x0000800A
GAME_MODE = 0x0000800B


def parse_property(text):
    """Xenia Property: id (LE u32), type byte at 4, value (BE) at 12, extra at 20."""
    raw = base64.b64decode(text, validate=True)
    if base64.b64encode(raw).decode() != text or len(raw) < 12:
        raise HttpError(500, "Invalid base64")
    pid = struct.unpack_from("<i", raw, 0)[0] & 0xFFFFFFFF
    ptype = raw[4]
    data = raw[20:] if ptype in (4, 6) else raw[12:]
    return pid, ptype, data


def context_value(prop_list, cid):
    for text in prop_list:
        pid, ptype, data = parse_property(text)
        if pid == cid and len(data) >= 4:
            return struct.unpack_from(">I", data, 0)[0]
    return None


def is_host(flags):
    return bool(flags & HOST) or flags == STATS or flags == SINGLEPLAYER_WITH_STATS


class HttpError(Exception):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status
        self.message = message


ERROR_NAMES = {400: "Bad Request", 403: "Forbidden", 404: "Not Found", 500: "Internal Server Error"}


def need(value, pattern, what):
    if not isinstance(value, str) or not pattern.match(value):
        raise HttpError(500, f"Invalid {what} {value}")  # the server throws a plain Error -> 500
    return value


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.players = {}   # xuid (upper) -> dict
        self.sessions = {}  # (title, sid lower) -> dict
        self.qos = {}       # (title, sid lower) -> bytes


STATE = State()


def player_by_ip(ip):
    for player in STATE.players.values():
        if player["hostAddress"] == ip:
            return player
    return None


def available(session):
    used_private = sum(1 for p in session["players"].values() if p)
    used_public = sum(1 for p in session["players"].values() if not p)
    return (max(0, session["publicSlotsCount"] - used_public),
            max(0, session["privateSlotsCount"] - used_private))


def session_dto(session, details=False, with_title=False):
    open_public, open_private = available(session)
    dto = {
        "id": session["id"],
        "flags": session["flags"],
        "hostAddress": session["hostAddress"],
        "macAddress": session["macAddress"],
        "publicSlotsCount": session["publicSlotsCount"],
        "privateSlotsCount": session["privateSlotsCount"],
        "openPublicSlotsCount": open_public,
        "openPrivateSlotsCount": open_private,
        "filledPublicSlotsCount": session["publicSlotsCount"] - open_public,
        "filledPrivateSlotsCount": session["privateSlotsCount"] - open_private,
        "port": session["port"],
    }
    if with_title:
        dto.update({"title": session["title"], "version": session["version"],
                    "mediaId": session["mediaId"], "xuid": session["xuid"]})
    if details:
        dto["players"] = [{"xuid": x} for x in session["players"]]
    return dto


class Handler(BaseHTTPRequestHandler):
    server_version = "MockXeniaWebServices/1.0"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        if self.server.verbose:
            super().log_message(fmt, *args)

    # --- plumbing ---------------------------------------------------------
    def client_ip(self):
        return self.client_address[0]

    def body_bytes(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def json_body(self):
        raw = self.body_bytes()
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip()
        if ctype != "application/json":
            return {}  # express.json() ignores other content types
        try:
            return json.loads(raw or b"{}")
        except ValueError:
            raise HttpError(400, "Unexpected token in JSON")

    def send(self, status, payload=None, raw=None, ctype="application/json"):
        data = raw if raw is not None else (b"" if payload is None else json.dumps(payload).encode())
        self.send_response(status)
        if data or raw is not None:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)

    def query(self, name):
        from urllib.parse import parse_qs, urlparse
        values = parse_qs(urlparse(self.path).query).get(name)
        return values[0] if values else None

    def handle_any(self, method):
        path = self.path.split("?")[0].rstrip("/") or "/"
        try:
            with STATE.lock:
                result = self.route(method, path)
        except HttpError as e:
            self.send(e.status, {"message": e.message, "error": ERROR_NAMES.get(e.status, "Error"),
                                 "statusCode": e.status})
            return
        status, payload, raw, ctype = result
        self.send(status, payload, raw, ctype)

    def do_GET(self):
        self.handle_any("GET")

    def do_HEAD(self):
        self.handle_any("HEAD")

    def do_POST(self):
        self.handle_any("POST")

    def do_DELETE(self):
        self.handle_any("DELETE")

    # --- routes -------------------------------------------------------------
    def route(self, method, path):
        ok_post = 201  # NestJS default for POST
        parts = path.strip("/").split("/") if path != "/" else []

        if method in ("GET", "HEAD") and not parts:
            return 200, None, b"<html><body>mock server</body></html>", "text/html"
        if method == "GET" and parts == ["whoami"]:
            return 200, {"address": self.client_ip()}, None, None

        if parts[:1] == ["DeleteSessions"] and method == "DELETE" and len(parts) <= 2:
            mac = parts[1].upper() if len(parts) == 2 and HEX12.match(parts[1]) else None
            host = self.query("hostAddress") or self.client_ip()
            for key, s in list(STATE.sessions.items()):
                if s["hostAddress"] == host and (mac is None or s["macAddress"] == mac):
                    del STATE.sessions[key]
            return 200, None, None, None

        if parts[:1] == ["players"]:
            if method == "POST" and len(parts) == 1:
                req = self.json_body()
                xuid = need(req.get("xuid"), HEX16, "Xuid:").upper()
                need(req.get("machineId"), HEX16, "Xuid:")
                need(req.get("macAddress"), HEX12, "MAC Address")
                STATE.players[xuid] = {
                    "xuid": xuid,
                    "gamertag": req.get("gamertag", ""),
                    "hostAddress": req.get("hostAddress", ""),
                    "machineId": req["machineId"].upper(),
                    "macAddress": req["macAddress"].upper(),
                    "port": 36000,
                    "sessionId": "0" * 16,
                    "settings": req.get("settings", {}),
                }
                return ok_post, None, None, None
            if method == "GET" and parts[1:] == ["deletemyprofiles"]:
                mine = [p for p in STATE.players.values() if p["hostAddress"] == self.client_ip()]
                for player in mine:
                    del STATE.players[player["xuid"]]
                return 200, [[p["gamertag"], p["xuid"]] for p in mine], None, None
            if method == "POST" and parts[1:] == ["find"]:
                req = self.json_body()
                player = player_by_ip(req.get("hostAddress"))
                if not player:
                    raise HttpError(404, "Player not found.")
                return ok_post, {k: player[k] for k in ("xuid", "gamertag", "hostAddress", "machineId",
                                                        "port", "macAddress", "sessionId")}, None, None

        if parts[:1] == ["title"] and len(parts) >= 3:
            title = parts[1].upper()
            if parts[2] == "ports" and len(parts) == 3 and method == "GET":
                return 200, {}, None, None
            if parts[2] == "sessions":
                return self.route_sessions(method, title, parts[3:])

        raise HttpError(404, f"Cannot {method} {path}")

    def route_sessions(self, method, title, rest):
        ok_post = 201

        if not rest and method == "POST":
            req = self.json_body()
            sid = need(req.get("sessionId"), HEX16, "SessionId").lower()
            flags = int(req.get("flags", 0))
            if is_host(flags):
                xuid = req.get("xuid")
                player = STATE.players.get(xuid.upper()) if xuid else player_by_ip(req.get("hostAddress"))
                if not player:
                    raise HttpError(403, f"Player not found: {xuid or req.get('hostAddress')}")
                STATE.sessions[(title, sid)] = {
                    "id": sid, "xuid": (xuid or player["xuid"]).upper(), "title": req.get("title", ""),
                    "mediaId": req.get("mediaId", ""), "version": req.get("version", ""),
                    "flags": flags, "hostAddress": req.get("hostAddress", ""),
                    "macAddress": need(req.get("macAddress"), HEX12, "MAC Address").upper(),
                    "publicSlotsCount": int(req.get("publicSlotsCount", 0)),
                    "privateSlotsCount": int(req.get("privateSlotsCount", 0)),
                    "port": int(req.get("port", 0)), "players": {}, "creator_ip": self.client_ip(),
                }
            return ok_post, None, None, None

        if rest == ["search"]:
            if method == "GET":
                return 200, [session_dto(s, with_title=True) for (t, _), s in STATE.sessions.items()
                             if t == title], None, None
            if method == "POST":
                req = self.json_body()
                searcher = (req.get("searcher_xuid") or "").upper()
                num_users = int(req.get("numUsers") or 0)
                results = []
                for (t, _), s in STATE.sessions.items():
                    open_public, open_private = available(s)
                    if t != title or s["xuid"] == searcher or s["publicSlotsCount"] == 0:
                        continue
                    if not (s["flags"] & MATCHMAKING) or (open_public == 0 and open_private == 0):
                        continue
                    if num_users and open_public < num_users and open_private < num_users:
                        continue
                    props = s.get("properties", [])
                    if not props or not any(parse_property(p)[1] == 0 for p in props):
                        continue  # no properties / contexts yet: not advertised
                    filters = req.get("filters")
                    if filters is not None:
                        want_mode = context_value(filters, GAME_MODE)
                        want_type = context_value(filters, GAME_TYPE)
                        if want_mode is None or want_type is None:
                            continue
                        if (context_value(props, GAME_MODE) != want_mode or
                                context_value(props, GAME_TYPE) != want_type):
                            continue
                    results.append(session_dto(s))
                count = int(req.get("resultsCount") or 0)
                return ok_post, results[:count] if count else results, None, None

        sid = rest[0].lower() if rest else ""
        if not HEX16.match(sid):
            raise HttpError(500, "Invalid SessionId " + sid)
        session = STATE.sessions.get((title, sid))
        action = rest[1] if len(rest) > 1 else ""

        if action == "qos":
            if method == "POST":
                ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip()
                raw = self.body_bytes()
                if ctype == "application/json":
                    raise HttpError(400, "Unexpected token in JSON")
                if ctype != "application/x-www-form-urlencoded":
                    raise HttpError(500, "rawBody is undefined")  # no parser ran
                STATE.qos[(title, sid)] = raw
                return ok_post, None, None, None
            if method == "GET":
                data = STATE.qos.get((title, sid))
                if data is None:
                    return 204, None, None, None
                return 200, None, data, "application/octet-stream"

        if method == "DELETE" and not action:
            if session and session["hostAddress"] == self.client_ip():
                del STATE.sessions[(title, sid)]
            return 200, None, None, None

        if not session:
            raise HttpError(404, f"Session {sid} was not found.")

        if action == "properties":
            if method == "POST" and len(rest) == 2:
                req = self.json_body()
                props = req.get("properties") or []
                for text in props:
                    parse_property(text)  # 500 on bad base64, like the server
                session["properties"] = list(props)
                return ok_post, None, None, None
            if method == "GET":
                # No XLast: everything except contexts, in stored order.
                return 200, {"properties": [p for p in session.get("properties", [])
                                            if parse_property(p)[1] != 0]}, None, None

        if method == "GET" and not action:
            return 200, session_dto(session), None, None
        if method == "GET" and action == "details":
            return 200, session_dto(session, details=True), None, None
        if method == "GET" and action == "arbitration":
            machines = {}
            for xuid in session["players"]:
                player = STATE.players.get(xuid)
                if player:
                    machines.setdefault(player["machineId"], []).append({"xuid": xuid})
            return 200, {"totalPlayers": sum(len(v) for v in machines.values()),
                         "machines": [{"id": k, "players": v} for k, v in machines.items()]}, None, None
        if method == "POST" and action == "modify":
            req = self.json_body()
            session["flags"] = int(req.get("flags", session["flags"]))
            session["publicSlotsCount"] = int(req.get("publicSlotsCount", 0))
            session["privateSlotsCount"] = int(req.get("privateSlotsCount", 0))
            return ok_post, None, None, None
        if method == "POST" and action in ("join", "prejoin"):
            req = self.json_body()
            xuids = [need(x, HEX16, "Xuid:").upper() for x in req.get("xuids", [])]
            if action == "join":
                private = req.get("privateSlots") or [False] * len(xuids)
                for xuid, is_private in zip(xuids, private):
                    session["players"][xuid] = bool(is_private)
            for xuid in xuids:
                if xuid in STATE.players:
                    STATE.players[xuid]["sessionId"] = sid
            return ok_post, None, None, None
        if method == "POST" and action == "leave":
            req = self.json_body()
            for xuid in req.get("xuids", []):
                session["players"].pop(need(xuid, HEX16, "Xuid:").upper(), None)
            return ok_post, None, None, None
        if method == "POST" and action == "migrate":
            req = self.json_body()
            if session["xuid"] in session["players"]:
                del session["players"][session["xuid"]]
            new_sid = sid[:2] + secrets.token_hex(7)
            new_session = dict(session, id=new_sid, players=dict(session["players"]),
                               hostAddress=req.get("hostAddress", ""),
                               macAddress=need(req.get("macAddress"), HEX12, "MAC Address").upper(),
                               port=int(req.get("port", 0)))
            if req.get("xuid"):
                new_session["xuid"] = need(req["xuid"], HEX16, "Xuid:").upper()
            del STATE.sessions[(title, sid)]
            STATE.sessions[(title, new_sid)] = new_session
            return ok_post, session_dto(new_session), None, None

        raise HttpError(404, f"Cannot {method} {self.path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=36000)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.verbose = args.verbose
    print(f"Mock Xenia Web Services on http://{args.host}:{args.port}/", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
