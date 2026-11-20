"""Run an isolated searcher and check its public HTTP responses."""

import json
from contextlib import ExitStack
from unittest import TestCase

from service_process import endpoints, http_request, start_service


class Searcher:
    def __init__(self, binary, index, stack, *, indexed=("hash", "from"),
                 flush_docs=2, merge_segments=16, router_config=None, extra_fields=()):
        fields = [{"name": name, "type": "VARCHAR", "stored": True, "indexed": True}
                  for name in indexed]
        fields.append({"name": "value", "type": "VARCHAR", "stored": True, "indexed": False})
        fields.extend(extra_fields)
        table = {"name": "transactions", "fields": fields,
                 "flush_docs": flush_docs, "merge_segments": merge_segments}
        (index / "schema.json").write_text(json.dumps({"tables": [table]}))
        self.origin, = endpoints(1)
        self.binary = binary
        self.index = index
        self.router_config = router_config
        self.process_stack = stack.enter_context(ExitStack())
        self.server = self._start()

    def _start(self):
        args = [self.index, self.origin]
        if self.router_config is not None:
            args.append(self.router_config)
        return start_service(self.process_stack, self.binary, self.origin, *args)

    def crash_and_reopen(self):
        self.server.kill()
        self.server.wait(timeout=5)
        self.process_stack.close()
        self.server = self._start()

    def post(self, path, payload, expected_status=200):
        body = {"table": "transactions", **payload}
        status, reply = http_request(self.origin, "POST", path, json.dumps(body),
                                     {"Content-Type": "application/json"})
        assert status == expected_status, (path, body, expected_status, status, reply)
        assert reply["ok"] == (status == 200), (path, reply)
        return reply

    def submit(self, path, height, rows, expected_status=200, *,
               match_fields=("hash",), accepted=None):
        reply = self.post(path, {"block_number": height, "match_field": match_fields,
                                 "documents": rows}, expected_status)
        if expected_status == 200:
            count = len(rows) if accepted is None else accepted
            assert reply == {"ok": True, "block_number": height, "upserted": count}, reply

    def expect_height(self, height):
        reply = self.post("/block_number", {})
        assert reply["block_number"] == height, (height, reply)

    def expect_hits(self, expression, *expected):
        hits = self.post("/search", {"query": expression})["hits"]
        # Keep multiplicity and compare decoded values, including 0 and 0.0.
        TestCase().assertCountEqual(hits, expected, msg=expression)
