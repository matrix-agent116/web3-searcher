"""Exercise built-in endpoints and configured query routes on the same URL."""
from contextlib import ExitStack
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.parse import urlencode
import json
import subprocess
import sys

from service_process import endpoints, http_request, start_service


def post(origin, path, payload):
    return http_request(origin, "POST", path, json.dumps(payload), {"Content-Type": "application/json"})


with TemporaryDirectory() as tmp, ExitStack() as stack:
    root, binaries = Path(tmp), Path(sys.argv[1]).resolve()
    schema = {"tables": [{"name": "internal_rows", "fields": [
        {"name": name, "type": "VARCHAR", "stored": name != "lookup", "indexed": name != "secret"}
        for name in ("key", "lookup", "secret")]}]}
    (root / "schema.json").write_text(json.dumps(schema))
    origin, = endpoints(1)
    route = {"path": "/v1/lookup", "methods": ["GET", "POST"], "table": "internal_rows",
             "params": {"needle": {"type": "string", "get": "q", "post": "/filter/value"}},
             "query": {"lucene": "(lookup:${needle} OR key:${needle})", "limit": 65535,
                       "select": {"id": "key"}}}
    files = root / "urls"
    files.mkdir()
    config = root / "router.conf"
    simple = {"path": "/v1/simple", "table": "internal_rows",
              "query": {"lucene": "(lookup:${value} OR key:${value})", "limit": 3,
                        "select": {"public_key": "key"}}}
    for name, spec in (("lookup", route), ("simple", simple),
                       ("broken", {**route, "path": "/v1/broken", "table": "missing_private_table"})):
        (files / f"{name}.json").write_text(json.dumps(spec))
    config.write_bytes(b"# public route files\r\n\nurls/lookup.json\r\n urls/simple.json \nurls/broken.json\n")
    start_service(stack, binaries / "searcher", origin, root, origin, config)
    rows = [{"key": str(i), "lookup": value, "secret": "private"}
            for i, value in enumerate(("ordinary", "AND", ' x:* OR lookup:* ', 'quote"/?+\\钱包', "404"))]
    wide_value = "x" * (18 * 1024)
    bulk = [{"key": f"bulk-{i}", "lookup": "bulk", "secret": wide_value} for i in range(1001)]
    for height, path in enumerate(("/solidity", "/candidate")):
        assert post(origin, path, {"table": "internal_rows", "block_number": height,
                                   "match_field": ["key"], "documents": rows[height::2] + bulk[height::2]})[0] == 200
    raw = {"table": "internal_rows", "query": "*:*", "fields": ["key"]}
    status, result = post(origin, "/search", raw)
    assert status == 200 and all(set(row) == {"key"} for row in result["hits"]), result
    for bad in ([], ["lookup"], ["missing"], ["key", "key"], "key"):
        assert post(origin, "/search", {**raw, "fields": bad, "query": "key:absent"})[0] == 400
    for limits, count in (({}, 100), ({"limit": 1}, 1), ({"limit": 750}, 750),
                          ({"limit": 1000}, 1000), ({"limit": 65535}, len(bulk))):
        status, reply = post(origin, "/search", {**raw, "query": "lookup:bulk", **limits})
        assert status == 200, reply
        assert len(reply["hits"]) == count, (limits, count, len(reply["hits"]))
        assert len({row["key"] for row in reply["hits"]}) == count
    for bad in (0, -1, 65536, 1.5, True, "2", None):
        assert post(origin, "/search", {**raw, "limit": bad})[0] == 400, bad
    wide_query = {**raw, "query": "lookup:bulk", "fields": ["secret"]}
    assert post(origin, "/search", {**wide_query, "limit": 1}) == (
        200, {"ok": True, "hits": [{"secret": wide_value}]})
    assert post(origin, "/search", {**wide_query, "limit": 65535})[0] == 413

    status, reply = http_request(origin, "GET", "/v1/lookup?q=bulk")
    assert status == 200 and len(reply["hits"]) == len(bulk)
    for row in rows:
        wanted = (200, {"ok": True, "hits": [{"id": row["key"]}]})
        assert http_request(origin, "GET", "/v1/lookup?" + urlencode({"q": row["lookup"]})) == wanted
        payload = {"filter": {"value": row["lookup"]}, "table": "secret", "query": "*:*", "fields": ["secret"]}
        assert post(origin, "/v1/lookup", payload) == wanted
    simple_hit = (200, {"ok": True, "hits": [{"public_key": "0"}]})
    assert post(origin, "/v1/simple", {"value": "ordinary"}) == simple_hit
    assert http_request(origin, "GET", "/v1/simple?value=ordinary") == simple_hit
    for status, reply in (post(origin, "/v1/simple", {"value": "bulk", "limit": 1000}),
                          http_request(origin, "GET", "/v1/simple?value=bulk&limit=1000")):
        assert status == 200 and len(reply["hits"]) == 3, reply
    for payload in ({}, {"filter": {"value": True}}, {"filter": {"value": ""}}):
        status, reply = post(origin, "/v1/lookup", payload)
        assert status == 400 and "/filter/value" in reply["error"]
    assert post(origin, "/block_number", {"table": "internal_rows"}) == (200, {"ok": True, "block_number": 0})
    status, result = post(origin, "/v1/broken", {"filter": {"value": "x"}})
    assert status == 500 and "missing_private_table" not in str(result) and "lookup" not in str(result)

    other = root / "other"
    other.mkdir()
    (other / "schema.json").write_text(json.dumps(schema))
    run = subprocess.run([binaries / "searcher", "serve", other, origin, config],
                         capture_output=True, text=True, timeout=5)
    assert run.returncode == 1 and "cannot listen" in run.stderr, run.stderr

    def rejects(manifest):
        config.write_text(manifest)
        run = subprocess.run([binaries / "searcher", "serve", root, *endpoints(1), config],
                             capture_output=True, timeout=3)
        assert run.returncode != 0 and b"router.conf" in run.stderr, (manifest, run.stderr)

    for pattern in ("lookup:${missing}", "${needle}:fixed", "lookup:${needle}*"):
        (files / "invalid.json").write_text(json.dumps({**route, "query": {
            **route["query"], "lucene": pattern}}))
        rejects("urls/invalid.json")
    (files / "invalid.json").write_text(json.dumps({**simple, "query": {**simple["query"], "limit": 65536}}))
    rejects("urls/invalid.json")
    for path in ("/candidate", "/solidity", "/block_number", "/search", "/health"):
        (files / "invalid.json").write_text(json.dumps({**simple, "path": path}))
        rejects("urls/invalid.json")
    (files / "invalid.json").write_text("{")
    rejects("urls/invalid.json")
    rejects("urls/simple.json\nurls/simple.json")
    rejects("urls/simple.json\nurls/missing.json")
    rejects("# no URLs")
