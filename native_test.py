"""Ejercita el servidor C sobre una configuración temporal en loopback."""

import http.client
import json
from pathlib import Path
import re
import socket
import stat
import subprocess
import tempfile
import time
from urllib.parse import urlencode

ORIGIN = "https://eden-rooms-ps5.pages.dev"
ORIGINAL = """{
  "multiplayer": {"host":"192.0.2.11", "port":1, "nickname":"original", "keep":[null, true, -1.2e+3]},
  "activeProfile": "actual",
  "profiles": {
    "actual": {"multiplayer":{"host":"192.0.2.12","port":2},"nickname":"activo"},
    "otro": {"multiplayer":{"host":"192.0.2.13","port":3,"nickname":"inactivo"}}
  },
  "otros": "host, port, nickname y \\"comillas\\""
}
"""


def check(path, port):
    self_origin = f"http://127.0.0.1:{port}"
    backup = path.with_name(path.name + ".edenrooms.bak")

    def request(method, route, status, *, origin=None, body=None, media=None, host=None, referer=None):
        headers = {}
        if origin is not None:
            headers["Origin"] = origin
        if media:
            headers["Content-Type"] = media
        if referer:
            headers["Referer"] = referer
        if host:
            headers["Host"] = host
        with_connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
        try:
            with_connection.request(method, route, body, headers)
            response = with_connection.getresponse()
            data = response.read().decode()
            assert response.status == status, (method, route, response.status, data)
            expected_origin = ORIGIN if origin == ORIGIN and status != 403 and route != "/v1/prepare" else None
            assert response.getheader("Access-Control-Allow-Origin") == expected_origin
            return data
        finally:
            with_connection.close()

    assert json.loads(request("GET", "/v1/health", 200))["service"] == "eden-bridge"
    request("GET", "/", 200)
    request("GET", "/", 403, host=f"rebind.test:{port}")
    assert json.loads(request("GET", "/v1/config-check", 200))["multiplayerSections"] == 2
    request("OPTIONS", "/v1/preview", 204, origin=ORIGIN)
    request("OPTIONS", "/v1/preview", 403, origin=ORIGIN + ".example")
    room = dict(host="192.0.2.100", port=24872, nick="Jugador_1")
    payload = json.dumps(room)
    for origin in (None, "null", "https://other.test", ORIGIN + ".example"):
        request("POST", "/v1/preview", 403, origin=origin, media="application/json", body=payload)
    for body in ('{"host":"192.0.2.100","port":0}', '{"host":"256.0.0.1","port":1}',
                 '{"host":"192.0.2.100","port":65536}', '{"host":"192.0.2.100","port":1.0}',
                 '{"host":"192.0.2.100","port":1,"port":2}',
                 '{"host":"192.0.2.100","port":1,"nick":"<script>"}', payload + " garbage"):
        request("POST", "/v1/preview", 400, origin=ORIGIN, media="application/json", body=body)
    result = request("POST", "/v1/preview", 200, origin=ORIGIN, media="application/json", body=payload)
    assert json.loads(result)["saved"] is False
    request("POST", "/v1/connection", 200, origin=ORIGIN, media="application/json", body=payload)
    request("POST", "/v1/connection", 409, origin=ORIGIN, media="text/plain", body=payload)
    request("GET", "/v1/prepare?host=192.0.2.1&port=1&port=2", 400)
    prepare = "/v1/prepare?" + urlencode(room)
    for referer in (None, ORIGIN + ".example/", "https://other.test/"):
        assert "data-auto='0'" in request("GET", prepare, 200, referer=referer)
    assert "data-auto='1'" in request("GET", prepare, 200, referer=self_origin + "/")
    rooms_origin = "https://eden-rooms.pages.dev"
    assert "data-auto='1'" in request("GET", prepare, 200, referer=rooms_origin + "/")
    for referer in (rooms_origin + ".example/", rooms_origin + "/other", "http://eden-rooms.pages.dev/"):
        assert "data-auto='0'" in request("GET", prepare, 200, referer=referer)
    page = request("GET", prepare, 200, referer=ORIGIN + "/")
    assert "data-auto='1'" in page
    token = re.search(r"name='confirm' value='([a-f0-9]{32})'", page)[1]
    media = "application/x-www-form-urlencoded"
    request("POST", "/v1/preview", 403, origin=self_origin, media=media, body=urlencode(room))
    submitted = urlencode(dict(room, confirm=token))
    request("POST", "/v1/preview", 403, origin=ORIGIN, media=media, body=submitted)
    assert path.read_text() == ORIGINAL and not backup.exists()

    assert json.loads(request("POST", "/v1/preview", 200, origin=self_origin,
                              media=media, body=submitted))["saved"] is True
    expected = ORIGINAL.replace('"192.0.2.11"', '"192.0.2.100"').replace('"192.0.2.12"', '"192.0.2.100"')
    expected = expected.replace('"port":1,', '"port":24872,').replace('"port":2}', '"port":24872}')
    expected = expected.replace('"original"', '"Jugador_1"').replace('"activo"', '"Jugador_1"')
    assert path.read_text() == expected
    assert backup.read_text() == ORIGINAL
    assert stat.S_IMODE(path.stat().st_mode) == 0o640
    assert stat.S_IMODE(backup.stat().st_mode) == 0o600

    request("POST", "/v1/preview", 200, origin=self_origin, media=media, body=submitted)
    assert backup.read_text() == ORIGINAL

    retry_page = request("GET", prepare, 200)
    retry_token = re.search(r"name='confirm' value='([a-f0-9]{32})'", retry_page)[1]
    assert retry_token != token
    retry = urlencode(dict(room, confirm=retry_token))
    for malformed in ('{"multiplayer":}', '{"multiplayer":{"host":"a","port":1}}',
                      ORIGINAL.replace('"actual",', '"ausente",'),
                      ORIGINAL.replace('"port":1,', '"port":1, "port":2,', 1)):
        path.write_text(malformed)
        request("GET", "/v1/config-check", 409)
        request("POST", "/v1/preview", 409, origin=self_origin, media=media, body=retry)
        assert path.read_text() == malformed and backup.read_text() == ORIGINAL

    path.unlink()
    path.symlink_to(backup)
    request("POST", "/v1/preview", 409, origin=self_origin, media=media, body=retry)
    assert path.is_symlink() and backup.read_text() == ORIGINAL
    path.unlink()
    path.write_text(expected)
    request("POST", "/v1/preview", 200, origin=self_origin, media=media, body=retry)
    assert backup.read_text() == expected
    assert list(path.parent.glob("*.tmp.*")) == []

    current = json.loads(ORIGINAL)
    current["profile_settings"] = current.pop("profiles")
    current["profiles"] = {"current": current.pop("activeProfile"), "first": "otro",
                           "users": {"1": "actual", "2": "otro"}}
    current["multiplayer"]["password"] = "preservar"
    current["profile_settings"]["actual"]["multiplayer"]["password"] = "preservar-activo"
    modern = json.dumps(current, indent=2) + "\n"
    page = request("GET", prepare, 200)
    token = re.search(r"name='confirm' value='([a-f0-9]{32})'", page)[1]
    submitted = urlencode(dict(room, confirm=token))
    for invalid, step in (
        (modern.replace('"current": "actual"', '"current": "ausente"'), "perfil-activo-ausente-o-ambiguo"),
        (modern.replace('"current": "actual"', '"current": "actual", "current": "otro"'), "selector-de-perfil"),
        (modern.replace('"profiles": {', '"activeProfile": "otro", "profiles": {'), "selector-de-perfil"),
    ):
        path.write_text(invalid)
        result = json.loads(request("POST", "/v1/preview", 409, origin=self_origin, media=media, body=submitted))
        assert result["step"] == step
        assert path.read_text() == invalid and backup.read_text() == expected

    path.write_text(modern)
    assert json.loads(request("GET", "/v1/config-check", 200))["multiplayerSections"] == 2
    request("POST", "/v1/preview", 200, origin=self_origin, media=media, body=submitted)
    for section in (current, current["profile_settings"]["actual"]):
        section["multiplayer"].update(host=room["host"], port=room["port"])
        nick_section = section if "nickname" in section else section["multiplayer"]
        nick_section["nickname"] = room["nick"]
    assert path.read_text() == json.dumps(current, indent=2) + "\n"
    assert backup.read_text() == modern

    raw_headers = [
        b"Origin: " + ORIGIN.encode() + b"\r\nOrigin: " + ORIGIN.encode(),
        b"Content-Length: 0\r\nContent-Length: 0",
        b"Transfer-Encoding: chunked",
        b"Content-Length: 2049",
        b"X-Test: x\0y",
    ]
    for header in raw_headers:
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.sendall(f"POST /v1/preview HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n".encode()
                         + header + b"\r\n\r\n")
            with sock.makefile("rb") as response:
                assert response.readline().startswith(b"HTTP/1.1 400")

    with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
        header = (f"POST /v1/preview HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                  f"Origin: {ORIGIN}\r\nContent-Type: application/json\r\n"
                  f"Content-Length: {len(payload)}\r\n\r\n")
        sock.sendall(header.encode() + payload[:5].encode())
        time.sleep(0.02)
        sock.sendall(payload[5:].encode())
        with sock.makefile("rb") as response:
            assert response.readline().startswith(b"HTTP/1.1 200")


def main():
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "prosperoeden.json"
        path.write_text(ORIGINAL)
        path.chmod(0o640)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        with subprocess.Popen(["build/eden-bridge-local", str(path), str(port)]) as process:
            try:
                for _ in range(100):
                    assert process.poll() is None
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except ConnectionRefusedError:
                        time.sleep(0.02)
                check(path, port)
            finally:
                process.terminate()


if __name__ == "__main__":
    main()
