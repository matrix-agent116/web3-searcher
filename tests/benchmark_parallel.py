"""Hold the second upsert open until the benchmark searches a committed hash."""

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
        elif self.path == "/upsert":
            height = body["block_number"]
            docs = body["documents"]
            if height == 0:
                self.server.first_block = {row["hash"] for row in docs}
            else:
                self.server.writing.set()
                if not self.server.searched.wait(3):
                    status, result = 503, {"ok": False, "error": "no concurrent search"}
                elif not self.server.reported.wait(5):
                    status, result = 503, {"ok": False, "error": "no live progress"}
            result.update(block_number=height, upserted=len(docs))
        elif self.path == "/search":
            if self.server.writing.wait(3) and body["value"] in self.server.first_block:
                if self.server.searched.is_set():
                    self.server.reported.wait(5)
                result["hits"] = [{"hash": body["value"]}]
                self.server.searched.set()
            else:
                status, result = 400, {"ok": False, "error": "expected a committed hash"}
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
    server.writing, server.searched, server.reported = Event(), Event(), Event()
    thread = Thread(target=server.serve_forever)
    thread.start()
    try:
        with subprocess.Popen([sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                               "--blocks", "2"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as run:
            saw_writes = saw_queries = False
            for line in run.stderr:
                if line.startswith("progress "):
                    sample = dict(item.split("=") for item in line.split()[1:])
                    saw_writes |= float(sample["tx/s"]) > 0 and int(sample["blocks"]) == 1
                    saw_queries |= float(sample["queries/s"]) > 0 and int(sample["queries"]) == 1
                    if saw_writes and saw_queries:
                        server.reported.set()
            assert run.wait(timeout=10) == 0
            result = json.load(run.stdout)
        assert result["transactions"] == 1024 and len(server.first_block) == 512
        assert 1 <= result["overlapping_searches"] <= result["search"]["requests"]
    finally:
        server.shutdown()
        thread.join()
