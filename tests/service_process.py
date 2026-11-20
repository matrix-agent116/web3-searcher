"""Start local service binaries and exercise their HTTP endpoints in tests."""

import http.client
import json
import socket
import subprocess
import time
from contextlib import ExitStack
from urllib.parse import urlsplit


def endpoints(count=3):
    with ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(count)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        return [f"http://127.0.0.1:{sock.getsockname()[1]}" for sock in sockets]


def http_request(origin, method, path, body=None, headers=None, chunked=False):
    url = urlsplit(origin)
    client = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
    try:
        client.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
        response = client.getresponse()
        assert response.getheader("Content-Type", "").startswith("application/json")
        return response.status, json.load(response)
    finally:
        client.close()


def start_service(stack, binary, endpoint, *arguments):
    proc = subprocess.Popen([binary, "serve", *arguments])

    def stop():
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)

    stack.callback(stop)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"{binary} exited with {proc.returncode}")
        try:
            assert http_request(endpoint, "GET", "/health") == (200, {"ok": True})
            return proc
        except ConnectionRefusedError:
            time.sleep(0.05)
    raise RuntimeError(f"{binary} startup timed out")
