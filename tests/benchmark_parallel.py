"""Check concurrent candidate reads and the sixteen-block confirmation lag."""

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import subprocess
import sys
from threading import Event, Thread


class Indexer(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        status, result = 200, {"ok": True}
        if self.path == "/block_number":
            result["block_number"] = -1
        elif self.path in ("/candidate", "/solidity"):
            self.server.writes.append((self.path, body))
            height, docs = body["block_number"], body["documents"]
            if self.path == "/candidate":
                if height == 1:
                    self.server.writing.set()
                    if not self.server.reported.wait(5):
                        status, result = 503, {"ok": False, "error": "no live progress"}
                self.server.visible.update(row["hash"] for row in docs)
            result.update(block_number=height, upserted=len(docs))
        elif self.path == "/search":
            field, _, value = body["query"].partition(":")
            if self.server.writing.wait(3) and field == "hash" and value in self.server.visible:
                if self.server.searched.is_set():
                    self.server.reported.wait(5)
                result["hits"] = [{"hash": value}]
                self.server.searched.set()
            else:
                status, result = 400, {"ok": False, "error": "expected a visible hash"}
        else:
            status, result = 404, {"ok": False}
        reply = json.dumps(result).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)

    def log_message(self, *args):
        pass


with ThreadingHTTPServer(("127.0.0.1", 0), Indexer) as server:
    server.writes = []
    server.visible = set()
    server.writing, server.searched, server.reported = Event(), Event(), Event()
    thread = Thread(target=server.serve_forever)
    thread.start()
    try:
        with subprocess.Popen([sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                               "--blocks", "18"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as run:
            saw_writes = saw_queries = False
            for line in run.stderr:
                if line.startswith("progress "):
                    sample = dict(item.split("=") for item in line.split()[1:])
                    saw_writes |= float(sample["candidates/s"]) > 0 and int(sample["candidates"]) == 1 and int(sample["blocks"]) == 0
                    saw_queries |= float(sample["queries/s"]) > 0 and int(sample["queries"]) == 1
                    if saw_writes and saw_queries:
                        server.reported.set()
            assert run.wait(timeout=10) == 0
            result = json.load(run.stdout)
        expected = []
        for height in range(18):
            expected.append(("/candidate", height))
            if height >= 16:
                expected.append(("/solidity", height - 16))
        assert [(path, body["block_number"]) for path, body in server.writes] == expected
        candidates = {body["block_number"]: body for path, body in server.writes if path == "/candidate"}
        for path, body in server.writes:
            if path == "/solidity":
                assert body == candidates[body["block_number"]]
        assert result["candidate"]["requests"] == 18 and result["solidity"]["requests"] == 2
        assert result["transactions"] == 1024 and len(server.visible) == 18 * 512
        assert 1 <= result["overlapping_searches"] <= result["search"]["requests"]
    finally:
        server.shutdown()
        thread.join()
