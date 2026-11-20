"""Exercise ambiguous writes and transient HTTP failures through the benchmark CLI."""

from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import subprocess
import sys
from threading import Thread


class Indexer(BaseHTTPRequestHandler):
    def do_POST(self):
        state = self.server
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        state.calls[self.path] += 1
        attempt = state.calls[self.path]
        status, reply = 200, {"ok": True}
        if self.path == "/block_number":
            if (state.case == "retry" and attempt == 1) or state.case == "exhaust":
                status = 504
            else:
                reply["block_number"] = state.height
        elif self.path == "/candidate":
            if attempt == 1:
                state.candidate = body
            else:
                assert body == state.candidate
            if state.case == "retry" and attempt == 1:
                self.close_connection = True
                return
            if state.case == "candidate_exhaust":
                status = 504
            else:
                count = 0 if state.case == "candidate_ignored" else len(body["documents"])
                reply.update(block_number=body["block_number"], upserted=count)
        elif self.path == "/solidity":
            if attempt > 1:
                assert body == state.first_request
            else:
                state.first_request = body
            if state.case == "invalid":
                status, reply = 400, {"ok": False, "error": "invalid document"}
            elif attempt == 1 and state.case in ("retry", "race"):
                status = 408
            elif state.case == "race" and attempt == 2:
                state.rows, state.height = body["documents"], body["block_number"]
                status, reply = 400, {"ok": False, "error": "block already finalized"}
            else:
                state.rows, state.height = body["documents"], body["block_number"]
                if state.case in ("lost", "conflict"):
                    if state.case == "conflict":
                        state.rows[-1]["value"] = "different"
                    self.close_connection = True
                    return
                reply.update(block_number=state.height, upserted=len(state.rows))
        elif self.path == "/search":
            if state.case == "retry" and attempt == 1:
                status = 504
            else:
                field, _, value = body["query"].partition(":")
                assert field == "hash"
                visible = state.rows + state.candidate["documents"]
                reply["hits"] = [row for row in visible if row["hash"] == value]
        else:
            status = 404
        data = json.dumps(reply).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


with ThreadingHTTPServer(("127.0.0.1", 0), Indexer) as server:
    thread = Thread(target=server.serve_forever)
    thread.start()
    try:
        for case in ("retry", "lost", "race", "conflict", "invalid", "exhaust",
                     "candidate_exhaust", "candidate_ignored"):
            server.case, server.calls, server.rows, server.height = case, Counter(), [], 4
            run = subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                                  "--blocks", "1", "--tx-per-block", "2", "--queries", "1"],
                                 capture_output=True, text=True, timeout=10)
            if case in ("retry", "lost", "race"):
                assert run.returncode == 0, (case, run.stderr)
                assert not any(line.startswith("round=") for line in run.stderr.splitlines())
                assert "query_summary type=hash requests=1 " in run.stderr, run.stderr
                report = json.loads(run.stdout)
                assert report["candidate"]["requests"] == report["solidity"]["requests"] == report["search"]["requests"] == 1
                assert report["transactions"] == 2 and report["first_block"] == report["last_block"] == 21
                assert report["last_solidity_block"] == server.height == 5
            else:
                assert run.returncode != 0 and not run.stdout, (case, run.stdout)
            if case == "retry":
                assert server.calls["/candidate"] == server.calls["/solidity"] == server.calls["/search"] == 2
                assert server.candidate["block_number"] == server.first_request["block_number"] + 16
                assert all(row["blockNumber"] == "0x5" for row in server.first_request["documents"])
            if case in ("lost", "invalid", "conflict"):
                assert server.calls["/solidity"] == 1
            if case == "race":
                assert server.calls["/solidity"] == 2
            if case in ("candidate_exhaust", "candidate_ignored"):
                attempts = 4 if case == "candidate_exhaust" else 1
                assert server.calls == {"/block_number": 1, "/candidate": attempts}
            if case == "exhaust":
                assert server.calls == {"/block_number": 4}
    finally:
        server.shutdown()
        thread.join()
