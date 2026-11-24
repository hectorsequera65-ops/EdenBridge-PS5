"""Comprueba las rutas reconstruidas con un servidor efímero en loopback."""

import http.client
import json
from http.server import HTTPServer
from threading import Thread

from bridge import BridgeHandler


def check():
    with HTTPServer(("127.0.0.1", 0), BridgeHandler) as server:
        server.allowed_origin = "https://example.test"
        worker = Thread(target=server.serve_forever)
        worker.start()
        try:
            connection = http.client.HTTPConnection(*server.server_address, timeout=5)
            try:
                for method, path, origin, body, expected in [
                    ("GET", "/v1/health", None, None, 200),
                    ("GET", "/v1/prepare?host=192.0.2.1&port=24872", None, None, 200),
                    ("OPTIONS", "/v1/connection", server.allowed_origin, None, 204),
                    ("POST", "/v1/connection", "https://other.test", "{}", 403),
                    ("POST", "/v1/connection", None, "{}", 403),
                    ("POST", "/v1/connection", server.allowed_origin, '{"host":"192.0.2.1","port":0}', 400),
                    ("POST", "/v1/preview", server.allowed_origin, '{"host":"192.0.2.1","port":24872}', 200),
                    ("POST", "/v1/connection", server.allowed_origin, '{"host":"192.0.2.1","port":24872}', 200),
                ]:
                    headers = {"Origin": origin} if origin else {}
                    connection.request(method, path, body, headers)
                    response = connection.getresponse()
                    data = response.read()
                    assert response.status == expected, (path, response.status, data)
                    if expected == 200:
                        assert json.loads(data)["writeEnabled"] is False
                    if method != "GET" and origin == server.allowed_origin:
                        assert response.getheader("Access-Control-Allow-Origin") == origin
            finally:
                connection.close()
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    check()
