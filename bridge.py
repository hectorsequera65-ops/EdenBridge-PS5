"""Reconstrucción parcial del protocolo de Eden Bridge para pruebas locales."""

import argparse
import ipaddress
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlsplit

DEFAULT_ORIGIN = "https://eden-rooms-ps5.pages.dev"


class BridgeHandler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def reply(self, status, body, *, cors=False):
        data = json.dumps(body).encode("utf-8") if body is not None else b""
        self.send_response(status)
        if cors:
            self.send_header("Access-Control-Allow-Origin", self.server.allowed_origin)
            self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Vary", "Origin")
        if data:
            self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        if data:
            self.wfile.write(data)

    def do_GET(self):
        request = urlsplit(self.path)
        if request.path == "/v1/health":
            self.reply(200, {"ok": True, "service": "eden-bridge", "writeEnabled": False})
            return
        if request.path == "/v1/prepare":
            query = parse_qs(request.query, keep_blank_values=True)
            try:
                if len(query["host"]) != 1 or len(query["port"]) != 1:
                    raise ValueError
                room = self.room(query["host"][0], int(query["port"][0]))
            except (KeyError, ValueError):
                self.reply(400, {"ok": False, "error": "invalid room"})
                return
            self.reply(200, room)
            return
        self.reply(404, {"ok": False, "error": "unknown route"})

    def check_post(self):
        if self.path not in ("/v1/connection", "/v1/preview"):
            self.reply(404, {"ok": False, "error": "unknown route"})
            return False
        if self.headers.get_all("Origin") != [self.server.allowed_origin]:
            self.reply(403, {"ok": False, "error": "origin denied"})
            return False
        return True

    def do_OPTIONS(self):
        if self.check_post():
            self.reply(204, None, cors=True)

    @staticmethod
    def room(host, port):
        if type(host) is not str or type(port) is not int or not 1 <= port <= 65535:
            raise ValueError
        return {
            "ok": True,
            "host": str(ipaddress.IPv4Address(host)),
            "port": port,
            "writeEnabled": False,
        }

    def do_POST(self):
        if not self.check_post():
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 1 <= length <= 2048:
                raise ValueError
            body = json.loads(self.rfile.read(length))
            room = self.room(body["host"], body["port"])
        except (KeyError, TypeError, ValueError, RecursionError):
            self.reply(400, {"ok": False, "error": "invalid room"}, cors=True)
            return
        self.reply(200, room, cors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=24879)
    parser.add_argument("--origin", default=DEFAULT_ORIGIN)
    args = parser.parse_args()
    origin = urlsplit(args.origin)
    if origin.scheme not in {"http", "https"} or not origin.netloc or args.origin != f"{origin.scheme}://{origin.netloc}" or any(not 33 <= ord(c) <= 126 for c in args.origin):
        parser.error("--origin debe ser un origen HTTP o HTTPS sin ruta")
    with HTTPServer(("127.0.0.1", args.port), BridgeHandler) as server:
        server.allowed_origin = args.origin
        server.serve_forever()


if __name__ == "__main__":
    main()
