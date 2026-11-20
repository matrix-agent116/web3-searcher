"""Check concurrent candidate reads and the sixteen-block confirmation lag."""

from collections import Counter
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
                self.server.rows = self.server.rows + docs
            result.update(block_number=height, upserted=len(docs))
        elif self.path == "/search":
            field, _, value = body["query"].partition(":")
            if self.server.writing.wait(3) and field == "hash" and value in self.server.visible:
                result["hits"] = [{"hash": value}]
            else:
                status, result = 400, {"ok": False, "error": "expected a visible hash"}
        elif self.path in ("/v1/account/received-stats", "/v1/account/top-senders"):
            rows = [row for row in self.server.rows if row["to"] == body["address"]]
            assert rows, (self.path, body)
            if self.path.endswith("received-stats"):
                amounts = [int(row["value"]) for row in rows]
                result["hits"] = [{"transfer_count": len(rows), "value_count": len(rows),
                                   "total_wei": str(sum(amounts)), "average_wei": str(sum(amounts) // len(rows)),
                                   "min_wei": str(min(amounts)), "max_wei": str(max(amounts))}]
            else:
                counts = Counter(row["from"] for row in rows)
                result["hits"] = [{"sender": addr, "transfer_count": count}
                                  for addr, count in sorted(counts.items(), key=lambda p: (-p[1], p[0]))[:10]]
        else:
            status, result = 404, {"ok": False}
        if "hits" in result:
            if self.server.searches == 3:
                self.server.reported.wait(5)
            self.server.searches += 1
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
    server.rows = []
    server.searches = 0
    server.writing, server.reported = Event(), Event()
    thread = Thread(target=server.serve_forever)
    thread.start()
    try:
        with subprocess.Popen([sys.argv[1], f"http://127.0.0.1:{server.server_port}",
                               "--blocks", "18"],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as run:
            saw_writes = False
            query_rates = set()
            summaries = {}
            for line in run.stderr:
                assert not line.startswith("round="), line
                if line.startswith("query_summary "):
                    fields = dict(part.split("=") for part in line.split()[1:])
                    summaries[fields["type"]] = fields
                if line.startswith("progress"):
                    assert line.rstrip() == "progress", line
                    assert next(run.stderr).split() == ["metric", "total", "rate/s"]
                    totals, rates = {}, {}
                    for metric in ("candidate_blocks", "confirmed_blocks", "confirmed_tx", "queries"):
                        label, total, rate = next(run.stderr).split()
                        assert label == metric
                        totals[metric], rates[metric] = int(total), float(rate)
                    saw_writes |= (rates["candidate_blocks"] > 0 and totals["candidate_blocks"] == 1
                                   and totals["confirmed_blocks"] == 0 and totals["confirmed_tx"] == 0)
                    assert next(run.stderr).strip() == ""
                    assert next(run.stderr).split() == ["query", "QPS", "avg_ms"]
                    for kind in ("hash", "received_stats", "top_senders"):
                        label, qps, avg_ms = next(run.stderr).split()
                        assert label == kind
                        if float(qps) > 0:
                            assert float(avg_ms) > 0
                            query_rates.add(kind)
                        else:
                            assert avg_ms == "-"
                    assert next(run.stderr).strip() == ""
                    if saw_writes and totals["queries"] == 3 and len(query_rates) == 3:
                        server.reported.set()
            assert run.wait(timeout=10) == 0
            result = json.load(run.stdout)
        for kind, stats in result["query_types"].items():
            summary = summaries[kind]
            assert int(summary["requests"]) == stats["requests"]
            assert float(summary["qps"]) > 0
            assert 0 < float(summary["p50_ms"]) <= float(summary["p95_ms"]) <= float(summary["p99_ms"])
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
        assert all(phase["requests"] > 0 for phase in result["query_types"].values())
        assert 3 <= result["overlapping_searches"] <= result["search"]["requests"]
    finally:
        server.shutdown()
        thread.join()
