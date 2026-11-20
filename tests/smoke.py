"""Exercise all three executables and the on-disk index via their public APIs."""

import fcntl
import http.client
import json
from itertools import count
import os
from pathlib import Path
from urllib.parse import urlsplit
import socket
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from contextlib import ExitStack

BIN = Path(sys.argv[1]).resolve()
SAMPLE = Path(__file__).resolve().parents[1] / "examples" / "transactions.json"


def endpoints():
    with ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(3)]
        for sock in sockets:
            sock.bind(("127.0.0.1", 0))
        return [f"http://127.0.0.1:{sock.getsockname()[1]}" for sock in sockets]


def http_request(origin, method, path, body=None, headers=None, chunked=False):
    url = urlsplit(origin)
    client = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
    try:
        client.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
        response = client.getresponse()
        assert response.getheader("Content-Type").startswith("application/json")
        return response.status, json.load(response)
    finally:
        client.close()


def segment_entries(manifest):
    header, *segments = manifest.read_text().splitlines()
    assert header.startswith("block_number ")
    return segments


def block_number(origin, table):
    status, result = http_request(origin, "POST", "/block_number",
                                  json.dumps({"table": table}), {"Content-Type": "application/json"})
    assert status == 200, result
    return result["block_number"]


def start(stack, name, endpoint, *arguments):
    proc = subprocess.Popen([BIN / name, "serve", *arguments])

    def stop():
        if proc.poll() is None:
            proc.terminate()
        proc.wait(timeout=10)

    stack.callback(stop)
    for _ in range(100):
        if proc.poll() is not None:
            raise RuntimeError(f"{name} exited with {proc.returncode}")
        try:
            assert http_request(endpoint, "GET", "/health") == (200, {"ok": True})
            return proc
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.05)
    raise RuntimeError(f"{name} startup timed out")


def cli(binary, *args, ok=True):
    run = subprocess.run([BIN / binary, *args], capture_output=True,
                         text=True, timeout=30)
    assert run.returncode == (0 if ok else 1), (args, run.stdout, run.stderr)
    result = json.loads(run.stdout)
    assert result["ok"] == ok, result
    return result


def prepare_catalog(index):
    index.mkdir()
    for name in ("schema.json", "transactions.json", "blocks.json"):
        (index / name).write_bytes(SAMPLE.with_name(name).read_bytes())


# Run the benchmark twice on the sample schema to check block boundaries,
# unique transaction hashes and resuming from a nonempty table.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work) / "index"
    prepare_catalog(index)
    origin = endpoints()[0]
    start(stack, "indexer", origin, index, origin)
    hashes = set()
    for first in (0, 2):
        stats = cli("indexer_benchmark", origin, "--blocks", "2", "--tx-per-block",
                    "3", "--queries", "7", "--query-threads", "2")
        assert (stats["first_block"], stats["last_block"]) == (first, first + 1)
        assert stats["transactions"] == 6 and stats["upsert"]["requests"] == 2
        assert stats["transactions_per_second"] > 0 and stats["search"]["requests"] == 7
        for phase in (stats["upsert"], stats["search"]):
            assert phase["requests_per_second"] > 0
            assert 0 < phase["p50_ms"] <= phase["p95_ms"] <= phase["p99_ms"]
        assert block_number(origin, "transactions") == first + 1
        for height in (first, first + 1):
            found = cli("searcher", "query", "transactions", "blockNumber", hex(height), origin)
            assert len(found["hits"]) == 3
            for row in found["hits"]:
                assert set(row) == {"hash", "from", "to", "blockNumber", "value"}
                for field, digits in (("hash", 64), ("from", 40), ("to", 40)):
                    if field == "to" and row[field] is None:
                        continue
                    assert row[field].startswith("0x") and len(row[field]) == digits + 2
                    int(row[field][2:], 16)
                assert row["blockNumber"] == hex(height) and int(row["value"], 16) >= 0
                hashes.add(row["hash"])
    assert len(hashes) == 12
    writes_only = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block",
                      "1", "--queries", "0")
    assert writes_only["last_block"] == 4 and writes_only["search"] is None
    # With no earlier block in this run, a single confirmed block ends
    # continuous mode immediately. An explicit query budget still runs in full.
    single = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block", "1")
    assert single["last_block"] == 5 and single["search"] is None
    single = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block", "1",
                 "--queries", "3")
    assert single["search"]["requests"] == 3 and single["overlapping_searches"] == 0
    invalid = subprocess.run([BIN / "indexer_benchmark", origin, "--blocks", "0"],
                             capture_output=True, timeout=10)
    assert invalid.returncode != 0 and block_number(origin, "transactions") == 6
    failed_write = subprocess.run([BIN / "indexer_benchmark", origin, "--tx-per-block",
                                   "100000"], capture_output=True, timeout=10)
    assert failed_write.returncode == 1 and b"16 MiB" in failed_write.stderr
    assert block_number(origin, "transactions") == 6
    # Reject a run that would overflow before writing even its first block.
    height = (1 << 64) - 2
    assert http_request(origin, "POST", "/upsert", json.dumps({
        "table": "transactions", "block_number": height, "match_field": ["hash"],
        "documents": []}), {"Content-Type": "application/json"})[0] == 200
    overflow = subprocess.run([BIN / "indexer_benchmark", origin, "--blocks", "2"],
                              capture_output=True, timeout=10)
    assert overflow.returncode != 0 and block_number(origin, "transactions") == height


with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    root = Path(work)
    index = root / "index"
    i, b, s = endpoints()
    prepare_catalog(index)
    schema_path = index / "schema.json"
    catalog = json.loads(schema_path.read_text())
    transactions = catalog["tables"][0]
    transactions.pop("flush_docs")  # Exercise the default document threshold.
    schema_path.write_text(json.dumps(catalog))
    indexer = start(stack, "indexer", i, index, i)
    start(stack, "updater", b, b, i)
    start(stack, "searcher", s, s, i)
    assert http_request(s, "GET", "/search")[0] == 405
    assert http_request(s, "POST", "/build")[0] == 404
    assert http_request(s, "POST", "/search", "{}")[0] == 415
    assert http_request(s, "POST", "/search", "{", {"Content-Type": "application/json"})[0] == 400
    assert http_request(i, "POST", "/index", "{}", {"Content-Type": "application/json"})[0] == 404
    rows = json.loads(SAMPLE.read_text())
    # The catalog is published before /health succeeds, without an update request.
    assert (index / "transactions" / "segments").is_file()
    assert http_request(b, "POST", "/build", "{}", {"Content-Type": "application/json"})[0] == 404
    for field, term, expected in (
        ("hash", rows[1]["hash"], rows[1:]),
        ("hash", "missing-transaction", []),
        ("from", rows[0]["from"], rows),
        ("from", "0x" + "A" * 40, []),
        ("to", rows[0]["to"], rows[:1]),
        ("blockNumber", "0x10", rows),
        ("blockNumber", "0xff", []),
    ):
        assert cli("searcher", "query", field, term, s)["hits"] == expected
    request = [b'{"field":"blockNumber",', b'"value":"0x10"}']
    assert http_request(s, "POST", "/search", iter(request), {"Content-Type": "application/json"},
                chunked=True) == (200, {"ok": True, "hits": rows})
    cli("searcher", "query", "unknown", "anything", s, ok=False)
    headers = {"Content-Type": "application/json"}
    next_height = count(2)
    def upsert(docs, table="transactions"):
        height = next(next_height)
        reply = http_request(b, "POST", "/upsert", json.dumps({
            "table": table, "block_number": height,
            "match_field": ["height" if table == "blocks" else "hash"], "documents": docs}), headers)
        if reply[0] == 200:
            assert reply[1] == {"ok": True, "block_number": height, "upserted": len(docs)}
        return reply

    changed = dict(rows[0], blockNumber="0x20", value="last")
    new = dict(rows[1], hash="new-transaction", blockNumber="0x30")
    batch = [dict(changed, blockNumber="0x19"), new, changed]
    source = root / "updates.json"
    source.write_text(json.dumps(batch))
    assert cli("updater", "update", "transactions", "1", '["hash"]', source, b) == {"ok": True, "block_number": 1, "upserted": 3}
    assert upsert(batch)[0] == 200
    assert cli("searcher", "query", "blockNumber", "0x10", s)["hits"] == rows[1:]
    assert cli("searcher", "query", "blockNumber", "0x19", s)["hits"] == []
    assert cli("searcher", "query", "hash", rows[0]["hash"], s)["hits"] == [changed]
    assert cli("searcher", "query", "hash", new["hash"], s)["hits"] == [new]
    # Hold the same directory lock a second indexer would use. While the
    # writer is waiting, another worker must still serve the committed row.
    workers = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count()
    if workers and workers > 1:
        replacement = dict(changed, value="after lock release")
        query = json.dumps({"field": "hash", "value": changed["hash"]})
        with ThreadPoolExecutor(max_workers=2) as clients:
            fd = os.open(index / "transactions", os.O_RDONLY)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX)
                write = clients.submit(upsert, [replacement])
                try:
                    write.result(timeout=0.2)
                except TimeoutError:
                    pass
                else:
                    raise AssertionError("update completed while directory was locked")
                search = clients.submit(http_request, i, "POST", "/search", query, headers)
                assert search.result(timeout=3) == (200, {"ok": True, "hits": [changed]})
            finally:
                os.close(fd)
            assert write.result()[0] == 200
        changed = replacement
        assert http_request(i, "POST", "/search", query, headers) == (200, {"ok": True, "hits": [changed]})
    # Both rows change in one commit. A single search must see one generation.
    pair = [dict(row, hash=f"snapshot-{n}", blockNumber="snapshot") for n, row in enumerate(rows)]
    def replace_pair(generation):
        docs = [dict(row, value=str(generation)) for row in pair]
        assert upsert(docs)[0] == 200

    replace_pair(0)
    with ThreadPoolExecutor(max_workers=1) as clients:
        def overwrite_pair():
            for generation in range(1, 33):
                replace_pair(generation)

        updates = clients.submit(overwrite_pair)
        while True:
            status, reply = http_request(i, "POST", "/search",
                                        json.dumps({"field": "blockNumber", "value": "snapshot"}), headers)
            assert status == 200
            hits = reply["hits"]
            assert [hit["hash"] for hit in hits] == [row["hash"] for row in pair]
            assert len({hit["value"] for hit in hits}) == 1
            if updates.done():
                break
        updates.result()
    # Fail manifest creation after the new segments have been flushed.
    table_path = index / "transactions"
    published = (table_path / "segments").read_bytes()
    files_before = set(table_path.iterdir())
    pending = table_path / "segments.pending"
    pending.mkdir()
    try:
        assert upsert([dict(changed, value="discard")] * 256)[0] >= 400
        assert (table_path / "segments").read_bytes() == published
        assert set(table_path.iterdir()) == files_before | {pending}
        assert cli("searcher", "query", "hash", rows[0]["hash"], s)["hits"] == [changed]
    finally:
        pending.rmdir()
    assert upsert([changed])[0] == 200
    assert upsert([dict(changed, value="discard"), {"hash": None}])[0] == 400
    assert upsert([{}])[0] == 400
    assert upsert([changed], "missing")[0] == 400
    assert cli("searcher", "query", "hash", rows[0]["hash"], s)["hits"] == [changed]
    # Sequential callers see every acknowledged single-row write. A checkpoint
    # packs hundreds of such requests, also preserving indexed-only fields.
    blocks_dir = index / "blocks"
    manifest = blocks_dir / "segments"
    seeded_segments = len(segment_entries(manifest))
    for n in range(520):
        doc = {"height": 1000 + n, "description": "single", "batch": n}
        assert upsert([doc], "blocks")[0] == 200
        query = json.dumps({"table": "blocks", "field": "batch", "value": str(n)})
        status, reply = http_request(i, "POST", "/search", query, headers)
        assert status == 200 and [hit["height"] for hit in reply["hits"] if hit["description"] == "single"] == [1000 + n]
        if n == 254:
            assert len(segment_entries(manifest)) == seeded_segments
    assert len(segment_entries(manifest)) == seeded_segments + 2
    # Leave the last eight rows in the WAL and simulate an interrupted append.
    indexer.kill()
    indexer.wait(timeout=10)
    wal, = blocks_dir.glob("updates.*.wal")
    with wal.open("ab") as interrupted:
        interrupted.write(b'[{"height":99999')
    assert http_request(s, "POST", "/search", json.dumps({"field": "hash", "value": rows[0]["hash"]}),
                {"Content-Type": "application/json"})[0] == 502
    tx_wal, = (index / "transactions").glob("updates.*.wal")
    buffered_docs = sum(len(json.loads(line)["documents"]) for line in tx_wal.read_text().splitlines())
    transactions["flush_docs"] = buffered_docs + 2
    schema_path.write_text(json.dumps(catalog))
    start(stack, "indexer", i, index, i)
    for n in (0, 255, 256, 511, 512, 519):
        assert cli("searcher", "query", "blocks", "height", str(1000 + n), s)["hits"][0]["height"] == 1000 + n
    assert cli("searcher", "query", "blocks", "height", "99999", s)["hits"] == []
    assert upsert([{"height": 99999}], "blocks")[0] == 200
    # Cover both the durable and buffered versions of one key at the next
    # checkpoint, then overwrite it again in memory.
    assert upsert([{"height": 1000, "description": "intermediate"}], "blocks")[0] == 200
    final = {"height": 1000, "description": "checkpointed"}
    assert upsert([final] * 256, "blocks")[0] == 200
    assert cli("searcher", "query", "blocks", "description", "intermediate", s)["hits"] == []
    assert cli("searcher", "query", "blocks", "description", "checkpointed", s)["hits"][0]["height"] == 1000
    assert upsert([dict(final, description="latest")], "blocks")[0] == 200
    assert cli("searcher", "query", "blocks", "description", "checkpointed", s)["hits"] == []
    hits = cli("searcher", "query", "blocks", "description", "single", s)["hits"]
    assert [hit["height"] for hit in hits] == list(range(1001, 1520))
    assert cli("searcher", "query", "hash", rows[0]["hash"], s)["hits"] == [changed]
    assert cli("searcher", "query", "hash", new["hash"], s)["hits"] == [new]
    assert cli("searcher", "query", "blockNumber", "0x10", s)["hits"] == rows[1:]

    # A large row remains buffered. The next row reaches the configured count.
    manifest = index / "transactions" / "segments"
    previous_count = len(segment_entries(manifest))
    large = dict(changed, hash="large", value="x" * (2 * 1024 * 1024))
    assert upsert([large])[0] == 200
    assert len(segment_entries(manifest)) == previous_count
    assert cli("searcher", "query", "hash", "large", s)["hits"] == [large]
    assert upsert([changed])[0] == 200
    assert len(segment_entries(manifest)) == previous_count + 1
    assert cli("searcher", "query", "hash", "large", s)["hits"] == [large]
    restored = cli("searcher", "query", "hash", changed["hash"], s)["hits"]
    assert restored == [changed], (restored, changed)

with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    root = Path(work)
    index = root / "catalog"
    i, b, s = endpoints()
    prepare_catalog(index)
    schema = index / "schema.json"
    config = json.loads(schema.read_text())
    transactions, blocks_table = config["tables"]
    schema.write_text(json.dumps({"tables": [transactions]}))
    assert cli("indexer", "build", index) == {"ok": True, "tables": 1, "indexed": 2}
    old_manifest = index / "transactions" / "segments"
    old_stat = old_manifest.stat()
    # An existing table must open even if its import source has disappeared.
    (index / "transactions.json").unlink()
    empty = {"name": "events", "fields": transactions["fields"]}
    blocks_table["flush_docs"] = 3
    analyzed = dict(blocks_table, name="analyzed")
    indexed_only = dict(blocks_table, name="indexed_only", flush_docs=2,
                       fields=[dict(f, stored=False) if f["name"] == "height" else f
                               for f in blocks_table["fields"]])
    schema.write_text(json.dumps({"tables": [transactions, blocks_table, empty, analyzed, indexed_only]}))
    (index / "blocks").mkdir()
    blocks = index / "blocks.json"
    block = json.loads(blocks.read_text())[0]
    blocks.write_text(json.dumps([dict(block, height="invalid")]))
    failed = subprocess.run([BIN / "indexer", "serve", index, i], capture_output=True, timeout=10)
    assert failed.returncode == 1
    assert list((index / "blocks").iterdir()) == []
    assert not list(index.glob(".table.*"))
    blocks.write_text(json.dumps([block]))
    # Startup opens transactions, fills the empty blocks directory and creates events.
    indexer = start(stack, "indexer", i, index, i)
    start(stack, "updater", b, b, i)
    start(stack, "searcher", s, s, i)
    assert all((index / name / "segments").is_file() for name in ("transactions", "blocks", "events"))
    assert (old_manifest.stat().st_ino, old_manifest.stat().st_mtime_ns) == (old_stat.st_ino, old_stat.st_mtime_ns)
    # An analyzed match field is still searchable by individual tokens.
    assert cli("searcher", "query", "analyzed", "description", "ethereum", s)["hits"][0]["height"] == block["height"]
    assert cli("searcher", "query", "blocks", "height", "016", s)["hits"] == []
    headers = {"Content-Type": "application/json"}
    assert http_request(b, "POST", "/update", '{"table":"events","documents":[{}]}', headers)[0] == 400
    keyed_batch = [dict(block, description=word) for word in ("overwritten", "current")]
    assert http_request(b, "POST", "/update", json.dumps({"table": "indexed_only", "block_number": 1, "match_field": ["height"], "documents": keyed_batch}), headers)[0] == 200
    assert len(cli("searcher", "query", "indexed_only", "height", str(block["height"]), s)["hits"]) == 1
    def check_indexed_only():
        assert cli("searcher", "query", "indexed_only", "description", "overwritten", s)["hits"] == []
        assert cli("searcher", "query", "indexed_only", "description", "ethereum", s)["hits"] == []
        hits = cli("searcher", "query", "indexed_only", "description", "current", s)["hits"]
        assert len(hits) == 1 and "height" not in hits[0]

    check_indexed_only()
    analyzed_docs = [dict(block, description="common alpha", height=11),
                     dict(block, description="common beta", height=12),
                     dict(block, description="common alpha", height=13)]
    assert http_request(b, "POST", "/update", json.dumps({"table": "analyzed", "block_number": 1, "match_field": ["description"], "documents": analyzed_docs}), headers)[0] == 200
    assert sorted(row["height"] for row in cli("searcher", "query", "analyzed", "description", "common", s)["hits"]) == [12, 13]
    def write_block(height, number=1):
        doc = dict(block, height=height, description="extra", metadata={"network":"test","tags":[]}, batch=9)
        return http_request(b, "POST", "/update", json.dumps({"table": "blocks", "block_number": number, "match_field": ["height"], "documents": [doc]}), headers)
    block_manifest = index / "blocks" / "segments"
    segment_count = len(segment_entries(block_manifest))
    with ThreadPoolExecutor(max_workers=4) as clients:
        writes = list(clients.map(write_block, range(100, 104)))
    assert sorted(status for status, _ in writes) == [200, 400, 400, 400], writes
    height = 1
    for h, (status, _) in zip(range(100, 104), writes):
        if status == 400:
            height += 1
            assert write_block(h, height)[0] == 200
    assert len(segment_entries(block_manifest)) == segment_count + 1
    for h in range(100, 104):
        assert cli("searcher", "query", "blocks", "height", str(h), s)["hits"][0]["height"] == h
    indexer.terminate()
    indexer.wait(timeout=10)
    blocks.unlink()
    start(stack, "indexer", i, index, i)
    check_indexed_only()
    assert cli("searcher", "query", "analyzed", "description", "alpha", s)["hits"][0]["height"] == 13
    expected = {key: value for key, value in block.items() if key != "batch"}
    for field, term in (("metadata.tags", "offline"), ("metadata.network", "ethereum"),
                        ("description", "ethereum"), ("height", "16"), ("batch", "1")):
        assert cli("searcher", "query", "blocks", field, term, s)["hits"] == [expected]
    assert cli("searcher", "query", "events", "hash", "xyz", s)["hits"] == []
    assert cli("searcher", "query", "transactions", "blockNumber", "0x10", s)["hits"] == rows
    cli("searcher", "query", "transactions", "value", "0x0", s, ok=False)
    cli("searcher", "query", "missing_table", "height", "16", s, ok=False)

    invalid_key = dict(block, height="not-an-integer")
    assert http_request(b, "POST", "/update", json.dumps({"table": "blocks", "block_number": 5, "match_field": ["height"], "documents": [invalid_key]}), headers)[0] == 400
    updated = dict(block, height="16", description="Updated")
    update_reply = http_request(b, "POST", "/update", json.dumps({"table": "blocks", "block_number": 5, "match_field": ["height"], "documents": [updated]}), headers)
    assert update_reply[0] == 200, update_reply
    assert cli("searcher", "query", "blocks", "description", "ethereum", s)["hits"] == []
    expected = dict(expected, description="Updated")

    def search_blocks(value):
        query = {"table": "blocks", "field": "metadata.tags", "value": value}
        return http_request(s, "POST", "/search", json.dumps(query), headers)

    terms = ["offline", "missing"] * 8
    with ThreadPoolExecutor(max_workers=8) as clients:
        for term, response in zip(terms, clients.map(search_blocks, terms)):
            assert response == (200, {"ok": True, "hits": [expected] if term == "offline" else []})

    damaged = root / "damaged"
    prepare_catalog(damaged)
    with ThreadPoolExecutor(max_workers=2) as clients:
        results = list(clients.map(lambda _: cli("indexer", "build", damaged), range(2)))
    assert sorted(report["indexed"] for report in results) == [0, 3], results
    broken_manifest = damaged / "transactions" / "segments"
    broken_manifest.write_text("corrupt")
    failure = subprocess.run([BIN / "indexer", "build", damaged], capture_output=True, timeout=10)
    assert failure.returncode != 0 and broken_manifest.read_text() == "corrupt"


# Verify the physical write path as well as the rows returned by both readers.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work) / "index"
    prepare_catalog(index)
    schema_path = index / "schema.json"
    catalog = json.loads(schema_path.read_text())
    tx_schema = catalog["tables"][0]
    tx_schema["flush_docs"] = 2
    # A seeded table with no indexed fields gives the native empty dictionary.
    catalog["tables"].append({"name": "columns", "documents": tx_schema["documents"],
                              "fields": [dict(field, indexed=False) for field in tx_schema["fields"]]})
    schema_path.write_text(json.dumps(catalog))
    origin = endpoints()[0]
    server = start(stack, "indexer", origin, index, origin)
    first, second = json.loads(SAMPLE.read_text())

    next_height = count(1)
    def write(docs, match_field="hash"):
        payload = json.dumps({"table": "transactions", "block_number": next(next_height), "match_field": [match_field], "documents": docs})
        reply = http_request(origin, "POST", "/upsert", payload, headers)
        assert reply[0] == 200, reply

    def hits(field, term):
        reply = http_request(origin, "POST", "/search", json.dumps({"field": field, "value": term}), headers)
        assert reply[0] == 200
        return reply[1]["hits"]

    # Reject a nonunique match atomically, including prior rows in the request.
    for field in ("from", "value", "unknown"):
        reply = http_request(origin, "POST", "/update", json.dumps({
            "table": "transactions", "block_number": 1, "match_field": [field],
            "documents": [dict(first, hash="uncommitted", **{"from": "elsewhere"}), first]
        }), headers)
        assert reply[0] == 400, reply
        assert hits("hash", "uncommitted") == []

    write([dict(second, value="patched")])
    assert hits("hash", second["hash"]) == [dict(second, value="patched")]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("from", second["from"]) == [first, dict(second, value="patched")]
    write([dict(first, value=None)])

    def latest_dictionary(table):
        directory = index / table
        last_id = int(segment_entries(directory / "segments")[-1].split()[0])
        return (directory / f"{last_id:016x}.idx").read_bytes()

    empty_dictionary = latest_dictionary("columns")
    assert latest_dictionary("transactions") == empty_dictionary
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("from", first["from"]) == [dict(first, value=None), dict(second, value="patched")]

    moved = dict(second, **{"from": "new-address", "value": "moved"})
    latest = dict(moved, value="latest")
    write([moved, latest])
    assert latest_dictionary("transactions") != empty_dictionary
    assert hits("from", first["from"]) == [dict(first, value=None)]
    assert hits("from", "new-address") == [latest]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("from", "new-address") == [latest]
    # Whole-row replacement clears omitted indexed fields.
    write([{"hash": second["hash"], "value": "cleared"}])
    assert hits("from", "new-address") == []
    # A patch followed by a full row in a mixed segment also survives replay.
    inserted = dict(first, hash="next-key")
    write([dict(first, value="after-restart"), inserted])
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("from", first["from"]) == [dict(first, value="after-restart"), inserted]

    # Change the match field across requests in one checkpoint. The old hash
    # must stay absent on replay even when a later request reuses that hash.
    other = dict(latest, hash="renamed", **{"from": "unique-address", "to": "unique-dest"})
    write([other])
    renamed = dict(other, hash="renamed-again")
    write([renamed], "to")
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("hash", "renamed") == []
    assert hits("hash", "renamed-again") == [renamed]
    patched = dict(renamed, value="updated-via-address")
    write([patched], "from")
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("hash", "renamed-again") == [patched]
    different = dict(other, **{"from": "other-address", "to": "other-dest"})
    write([different])
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", origin, index, origin)
    assert hits("hash", "renamed") == [different]
    assert hits("hash", "renamed-again") == [patched]


with tempfile.TemporaryDirectory(dir="/tmp") as work:
    index = Path(work)
    for bad_value in (0, -1, 1.5, "256", None):
        bad_table = dict(transactions, flush_docs=bad_value)
        (index / "schema.json").write_text(json.dumps({"tables": [bad_table]}))
        failure = subprocess.run([BIN / "indexer", "build", index], capture_output=True, text=True)
        assert failure.returncode == 1 and "flush_docs" in failure.stderr

    for bad_value in (0, 1, -1, 2.5, "3", None):
        bad_table = dict(transactions, merge_segments=bad_value)
        (index / "schema.json").write_text(json.dumps({"tables": [bad_table]}))
        failure = subprocess.run([BIN / "indexer", "build", index], capture_output=True, text=True)
        assert failure.returncode == 1 and "merge_segments" in failure.stderr


# Compact a disk prefix with masks and column patches while serving real HTTP
# requests. A pending WAL must still refer to the same logical checkpoint.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work) / "index"
    prepare_catalog(index)
    catalog = json.loads((index / "schema.json").read_text())
    schema = catalog["tables"][0]
    schema.update(flush_docs=2, merge_segments=3)
    (index / "schema.json").write_text(json.dumps(catalog))
    origin = endpoints()[0]
    server = start(stack, "indexer", origin, index, origin)
    directory = index / "transactions"
    manifest = directory / "segments"
    first, second = json.loads(SAMPLE.read_text())

    def update(docs, field="hash", *, height):
        payload = {"table": "transactions", "block_number": height, "match_field": [field], "documents": docs}
        reply = http_request(origin, "POST", "/update", json.dumps(payload), headers)
        assert reply == (200, {"ok": True, "block_number": height, "upserted": len(docs)}), reply

    def query(field, value):
        payload = {"table": "transactions", "field": field, "value": value}
        status, result = http_request(origin, "POST", "/search", json.dumps(payload), headers)
        assert status == 200, result
        return result["hits"]

    def await_merge(old_file, max_segments=1):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            assert query("to", first["to"]) == [patched]
            if not old_file.exists() and len(segment_entries(manifest)) <= max_segments:
                return
            time.sleep(0.025)
        raise AssertionError(f"merge did not reclaim {old_file}: {manifest.read_text()}")

    seed_file, = directory.glob("*.idx")
    patched = dict(first, value="stored-column-patch")
    changed = dict(second, to="new-destination")
    update([patched, changed], height=100)
    assert len(segment_entries(manifest)) == 2
    # Two visible documents may share a hash after an update by another field.
    duplicate = dict(changed, hash=first["hash"])
    added = dict(second, hash="durable", to="unique")
    update([duplicate, added], "to", height=101)
    hot = dict(added, hash="hot", to="wal-only")
    update([hot], height=102)
    await_merge(seed_file)
    assert block_number(origin, "transactions") == 102
    assert manifest.read_text().splitlines()[0] == "block_number 101"
    assert query("hash", first["hash"]) == [patched, duplicate]
    assert query("hash", second["hash"]) == []
    assert query("hash", "hot") == [hot]
    assert list(directory.glob("updates.*.wal"))
    server.kill()
    server.wait(timeout=10)
    # Simulate an interruption between the durable manifest rename and unlink.
    seed_file.touch()
    server = start(stack, "indexer", origin, index, origin)
    assert query("hash", first["hash"]) == [patched, duplicate]
    assert query("hash", "hot") == [hot]

    assert block_number(origin, "transactions") == 102
    assert not seed_file.exists()

    # Cross more checkpoints during compaction and compact an existing base.
    first_base, = directory.glob("*.idx")
    def more_updates():
        for n in range(24):
            update([dict(added, hash=f"concurrent-{n}")], height=110+n)
    with ThreadPoolExecutor(max_workers=1) as clients:
        writes = clients.submit(more_updates)
        while not writes.done():
            assert query("hash", first["hash"]) == [patched, duplicate]
        writes.result()
    await_merge(first_base, max_segments=2)
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", origin, index, origin)
    for n in range(24):
        assert query("hash", f"concurrent-{n}") == [dict(added, hash=f"concurrent-{n}")]

# Two tables own separate memory segments, counters, workers and WALs.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work)
    schema = json.loads(SAMPLE.with_name("schema.json").read_text())["tables"][0]
    schema.pop("documents")
    tables = [dict(schema, name=name, flush_docs=limit, merge_segments=2)
              for name, limit in (("fast", 3), ("slow", 17))]
    (index / "schema.json").write_text(json.dumps({"tables": tables}))
    origin = endpoints()[0]
    server = start(stack, "indexer", origin, index, origin)

    next_height = count(1)
    def batch(table, hashes):
        height = next(next_height)
        rows = [{"hash": key, "value": table} for key in hashes]
        status, result = http_request(origin, "POST", "/upsert", json.dumps({
            "table": table, "block_number": height, "match_field": ["hash"], "documents": rows}), headers)
        assert (status, result) == (200, {"ok": True, "block_number": height, "upserted": len(rows)})

    for table in ("fast", "slow"):
        batch(table, ["shared", "second"])
    batch("fast", ["shared"])
    for n in range(14):
        batch("slow", [f"row-{n}"])
    assert segment_entries(index / "fast" / "segments")
    assert segment_entries(index / "slow" / "segments") == []
    assert list((index / "slow").glob("updates.*.wal"))
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", origin, index, origin)
    for table in ("fast", "slow"):
        status, result = http_request(origin, "POST", "/search", json.dumps({
            "table": table, "field": "hash", "value": "shared"}), headers)
        assert status == 200 and [row["value"] for row in result["hits"]] == [table]
    batch("slow", ["shared", "last"])
    assert len(segment_entries(index / "slow" / "segments")) == 1
    assert not list((index / "slow").glob("updates.*.wal"))

# Match all components on the same live row, including an indexed-only field.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work)
    table = {"name": "logs", "flush_docs": 8, "merge_segments": 2,
             "fields": [{"name": name, "type": kind, "indexed": indexed, "stored": stored}
                        for name, kind, indexed, stored in [
                            ("chain", "BIGINT", True, False), ("hash", "VARCHAR", True, True),
                            ("tag", "VARCHAR", True, True), ("value", "VARCHAR", False, True)]]}
    (index / "schema.json").write_text(json.dumps({"tables": [table]}))
    origin = endpoints()[0]
    server = start(stack, "indexer", origin, index, origin)

    next_height = count(1)
    def submit(fields, docs, status=200):
        reply = http_request(origin, "POST", "/upsert", json.dumps({
            "table": "logs", "block_number": next(next_height), "match_field": fields, "documents": docs}), headers)
        assert reply[0] == status, (fields, reply)
        if status == 200:
            assert reply[1]["upserted"] == len(docs)

    def values(field, term):
        status, result = http_request(origin, "POST", "/search", json.dumps({
            "table": "logs", "field": field, "value": term}), headers)
        assert status == 200
        return sorted(row["value"] for row in result["hits"])

    key = ["chain", "hash"]
    a = dict(chain=1, hash="A", tag="first", value="one")
    b = dict(chain=2, hash="A", tag="second", value="two")
    c = dict(chain=1, hash="B", tag="third", value="three")
    submit(key, [a, b, c])
    submit(key[::-1], [dict(a, value="intermediate"), dict(a, value="patched")])
    assert values("chain", "1") == ["patched", "three"]
    assert values("hash", "A") == ["patched", "two"]
    for fields in ("hash", [], ["chain", "chain"], ["missing"], ["value"], [1], ["hash", None]):
        submit(fields, [], 400)
    for invalid in ({"chain": 1}, dict(a, chain=None)):
        submit(key, [dict(a, value="rolled back"), invalid], 400)
    submit(["hash"], [dict(a, hash="never"), a], 400)
    assert values("hash", "never") == []
    assert values("tag", "first") == ["patched"]
    wal, = (index / "logs").glob("updates.*.wal")
    assert [json.loads(line)["match_field"] for line in wal.read_text().splitlines()] == [key, key[::-1]]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert values("tag", "first") == ["patched"]

    changed = dict(a, tag="renamed", value="moved")
    submit(key, [changed, dict(b, chain=3, tag="fourth", value="four"), dict(changed, value="latest")])
    assert not wal.exists()
    assert values("hash", "A") == ["four", "latest", "two"]
    assert values("tag", "first") == []
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", origin, index, origin)
    assert values("chain", "1") == ["latest", "three"]
    submit(["tag"], [dict(c, hash="A")])
    submit(key, [dict(a, hash="never"), a], 400)
    assert values("hash", "never") == []
    submit(["tag", "hash", "chain"], [dict(changed, value="exact")])
    assert values("hash", "A") == ["exact", "four", "three", "two"]

# Published height follows successful batches, even between disk checkpoints.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work) / "index"
    prepare_catalog(index)
    catalog = json.loads((index / "schema.json").read_text())
    catalog["tables"][0].update(flush_docs=2, merge_segments=16)
    (index / "schema.json").write_text(json.dumps(catalog))
    origin, updater, searcher = endpoints()
    server = start(stack, "indexer", origin, index, origin)
    start(stack, "updater", updater, updater, origin)
    start(stack, "searcher", searcher, searcher, origin)
    directory = index / "transactions"

    def write(height, docs, expected_status=200):
        body = {"table": "transactions", "block_number": height,
                "match_field": ["hash"], "documents": docs}
        reply = http_request(updater, "POST", "/upsert", json.dumps(body), headers)
        assert reply[0] == expected_status, reply
        if expected_status == 200:
            assert reply[1] == {"ok": True, "upserted": len(docs), "block_number": height}

    assert block_number(searcher, "transactions") == -1
    assert (directory / "segments").read_text().splitlines()[0] == "block_number -1"
    write(-1, [], 400)
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(origin, "transactions") == -1
    write(0, [])
    assert block_number(searcher, "transactions") == 0
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(origin, "transactions") == 0
    write(0, [{"hash": "duplicate"}], 400)
    write(1, [{"hash": "first"}, {"hash": "second"}])
    assert (directory / "segments").read_text().splitlines()[0] == "block_number 1"
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(searcher, "transactions") == 1
    write(100, [{"hash": "a"}, {"hash": "b"}])
    assert not list(directory.glob("updates.*.wal"))
    write(101, [{"hash": "c", "value": "in memory"}])
    assert block_number(searcher, "transactions") == 101
    assert block_number(origin, "blocks") == -1
    wal, = directory.glob("updates.*.wal")
    assert [json.loads(line)["block_number"] for line in wal.read_text().splitlines()] == [101]
    pending_manifest = directory / "segments.pending"
    pending_manifest.mkdir()
    try:
        write(102, [{"hash": "uncommitted"}], 400)
        assert block_number(origin, "transactions") == 101
        assert (directory / "segments").read_text().splitlines()[0] == "block_number 100"
    finally:
        pending_manifest.rmdir()
    write(102, [{"value": "no hash"}], 400)
    write(99, [{"hash": "stale"}], 400)
    write(-1, [], 400)
    write("101", [], 400)
    write(None, [], 400)
    assert http_request(updater, "POST", "/upsert", json.dumps({
        "table": "transactions", "match_field": ["hash"], "documents": []}), headers)[0] == 400
    assert block_number(origin, "transactions") == 101

    server.kill()
    server.wait(timeout=10)
    with wal.open("ab") as truncated:
        truncated.write(b'{"block_number":999,"match_field":["hash"],"documents":[')
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(origin, "transactions") == 101
    assert cli("searcher", "query", "hash", "c", searcher)["hits"][0]["value"] == "in memory"
    assert wal.read_bytes().endswith(b"\n")
    write(101, [{"hash": "c", "value": "extra part"}], 400)
    write(102, [{"hash": "c", "value": "updated"}])
    assert not wal.exists()
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(origin, "transactions") == 102

    write(105, [])
    assert block_number(searcher, "transactions") == 105
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert block_number(searcher, "transactions") == 105
    assert cli("searcher", "query", "hash", "c", searcher)["hits"][0]["value"] == "updated"
    max_height = (1 << 64) - 1
    write(max_height, [{"hash": "x"}, {"hash": "y"}])
    assert block_number(searcher, "transactions") == max_height
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", origin, index, origin)
    assert block_number(origin, "transactions") == max_height

# A table publishes each full candidate or finalized block in one transition.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work)
    schema = json.loads(SAMPLE.with_name("schema.json").read_text())["tables"][0]
    schema.pop("documents")
    (index / "schema.json").write_text(json.dumps({"tables": [schema]}))
    i, u, s = endpoints()
    server = start(stack, "indexer", i, index, i)
    start(stack, "updater", u, u, i)
    start(stack, "searcher", s, s, i)
    hdr = {"Content-Type": "application/json"}

    def send(path, number, docs):
        return http_request(u, "POST", path, json.dumps({"table": "transactions",
                            "block_number": number, "match_field": ["hash"],
                            "documents": docs}), hdr)

    def hashes():
        status, reply = http_request(s, "POST", "/search", json.dumps(
            {"field": "from", "value": "sender"}), hdr)
        assert status == 200
        return sorted(row["hash"] for row in reply["hits"])

    def docs(*keys):
        return [{"hash": key, "from": "sender"} for key in keys]

    assert send("/candidate", 5, docs("later"))[0] == 200
    assert send("/candidate", 3, docs("a", "b"))[0] == 200
    assert hashes() == ["a", "b", "later"]
    assert send("/candidate", 3, docs("x", "y"))[0] == 200
    assert hashes() == ["later", "x", "y"]
    assert send("/candidate", 3, docs("invalid") + [{}])[0] == 400
    assert send("/upsert", 3, docs("invalid") + [{}])[0] == 400
    assert block_number(s, "transactions") == -1
    assert hashes() == ["later", "x", "y"]
    if workers and workers > 1:
        with ThreadPoolExecutor(max_workers=1) as clients:
            fd = os.open(index / "transactions", os.O_RDONLY)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX)
                confirmation = clients.submit(send, "/upsert", 3, docs("confirmed"))
                try:
                    confirmation.result(timeout=0.2)
                except TimeoutError:
                    pass
                else:
                    raise AssertionError("confirmation bypassed the directory lock")
                assert hashes() == ["later", "x", "y"]
            finally:
                os.close(fd)
            assert confirmation.result()[0] == 200
    else:
        assert send("/upsert", 3, docs("confirmed"))[0] == 200
    assert block_number(s, "transactions") == 3
    assert hashes() == ["confirmed", "later"]
    assert send("/upsert", 3, docs("second-part"))[0] == 400
    assert send("/candidate", 3, docs("late"))[1]["upserted"] == 0
    assert hashes() == ["confirmed", "later"]
    server.kill()
    server.wait(timeout=10)
    start(stack, "indexer", i, index, i)
    assert hashes() == ["confirmed"]
    assert block_number(s, "transactions") == 3
