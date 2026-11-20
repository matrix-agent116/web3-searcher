"""Exercise built-in endpoints and configured query routes on the same URL."""
from concurrent.futures import ThreadPoolExecutor
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
    schema["tables"][0]["fields"].append(
        {"name": "amount", "type": "DOUBLE", "stored": True, "indexed": False})
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
    count = {"op": "count", "field": "*"}
    ranking = {"path": "/v1/ranking", "table": "internal_rows",
               "query": {"lucene": "lookup:${value}", "group_by": ["secret"],
                         "select": {"label": "secret", "n": count},
                         "order_by": [{"field": "n", "direction": "desc"},
                                      {"field": "label", "direction": "asc"}], "limit": 2}}
    total = {"path": "/v1/total", "table": "internal_rows",
             "query": {"lucene": "lookup:${value}", "select": {"total": count}, "limit": 1}}
    metrics = {op: {"op": op, "field": "amount"} for op in ("count", "sum", "avg", "min", "max")}
    stats = {"path": "/v1/stats", "table": "internal_rows",
             "query": {"lucene": "lookup:${value}", "select": {"rows": count, **metrics}}}
    grouped = {"path": "/v1/grouped-stats", "table": "internal_rows",
               "query": {**stats["query"], "select": {"label": "secret", **metrics},
                         "group_by": ["secret"], "order_by": [{"field": "sum", "direction": "desc"}], "limit": 1}}
    with config.open("a") as manifest:
        for name, spec in (("ranking", ranking), ("total", total), ("stats", stats), ("grouped", grouped)):
            (files / f"{name}.json").write_text(json.dumps(spec))
            manifest.write(f"urls/{name}.json\n")
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

    def expect_hits(path, value, hits):
        wanted = (200, {"ok": True, "hits": hits})
        for reply in (post(origin, path, {"value": value}),
                      http_request(origin, "GET", path + "?" + urlencode({"value": value}))):
            assert reply == wanted, (path, value, reply, wanted)

    expect_hits("/v1/total", "bulk", [{"total": 1001}])
    expect_hits("/v1/total", "absent", [{"total": 0}])
    expect_hits("/v1/ranking", "absent", [])
    empty = {"count": 0, "sum": None, "avg": None, "min": None, "max": None}
    expect_hits("/v1/stats", "absent", [{"rows": 0, **empty}])
    expect_hits("/v1/stats", "bulk", [{"rows": 1001, **empty}])

    def submit_groups(path, labels):
        docs = [{"key": f"rank-{i}", "lookup": "rank", "secret": label, "amount": amount}
                for i, (label, amount) in enumerate(labels)]
        assert post(origin, path, {"table": "internal_rows", "block_number": 2,
                                   "match_field": ["key"], "documents": docs})[0] == 200

    submit_groups("/candidate", [("z", -3), ("b", None), ("a", 6), ("z", 11)])
    expect_hits("/v1/ranking", "rank", [{"label": "z", "n": 2}, {"label": "a", "n": 1}])
    expect_hits("/v1/grouped-stats", "rank",
                [{"label": "z", "count": 2, "sum": 8, "avg": 4, "min": -3, "max": 11}])
    # Exercise both aggregate connections with different filters and layouts.
    with ThreadPoolExecutor(max_workers=2) as workers:
        for _ in range(8):
            bulk_stats = workers.submit(post, origin, "/v1/stats", {"value": "bulk"})
            rank_stats = workers.submit(post, origin, "/v1/grouped-stats", {"value": "rank"})
            assert bulk_stats.result() == (200, {"ok": True, "hits": [{"rows": 1001, **empty}]})
            assert rank_stats.result() == (200, {"ok": True, "hits": [
                {"label": "z", "count": 2, "sum": 8, "avg": 4, "min": -3, "max": 11}]})
    submit_groups("/candidate", [(None, 1), ("a", 2), (None, 0)])
    expect_hits("/v1/ranking", "rank", [{"label": None, "n": 2}, {"label": "a", "n": 1}])
    expect_hits("/v1/stats", "rank", [{"rows": 3, "count": 3, "sum": 3, "avg": 1, "min": 0, "max": 2}])
    submit_groups("/solidity", [("confirmed", 3)])
    expect_hits("/v1/ranking", "rank", [{"label": "confirmed", "n": 1}])
    expect_hits("/v1/stats", "rank", [{"rows": 1, "count": 1, "sum": 3, "avg": 3, "min": 3, "max": 3}])

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
