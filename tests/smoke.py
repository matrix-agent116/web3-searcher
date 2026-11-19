"""Exercise all three executables and the on-disk index via their public APIs."""

import fcntl
import http.client
import json
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
    def upsert(docs, table="transactions"):
        return http_request(b, "POST", "/update", json.dumps({"table": table, "documents": docs}), headers)

    changed = dict(rows[0], blockNumber="0x20", value="last")
    new = dict(rows[1], hash="new-transaction", blockNumber="0x30")
    batch = [dict(changed, blockNumber="0x19"), new, changed]
    source = root / "updates.json"
    source.write_text(json.dumps(batch))
    assert cli("updater", "update", "transactions", source, b) == {"ok": True, "upserted": 3}
    assert upsert(batch) == (200, {"ok": True, "upserted": 3})
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
            assert write.result() == (200, {"ok": True, "upserted": 1})
        changed = replacement
        assert http_request(i, "POST", "/search", query, headers) == (200, {"ok": True, "hits": [changed]})
    # Both rows change in one commit. A single search must see one generation.
    pair = [dict(row, hash=f"snapshot-{n}", blockNumber="snapshot") for n, row in enumerate(rows)]
    def replace_pair(generation):
        docs = [dict(row, value=str(generation)) for row in pair]
        assert upsert(docs) == (200, {"ok": True, "upserted": len(pair)})

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
    assert upsert([changed]) == (200, {"ok": True, "upserted": 1})
    assert upsert([dict(changed, value="discard"), {"hash": None}])[0] == 400
    assert upsert([{}])[0] == 400
    assert upsert([changed], "missing")[0] == 400
    assert cli("searcher", "query", "hash", rows[0]["hash"], s)["hits"] == [changed]
    # Sequential callers see every acknowledged single-row write. A checkpoint
    # packs hundreds of such requests, also preserving indexed-only fields.
    blocks_dir = index / "blocks"
    manifest = blocks_dir / "segments"
    seeded_segments = len(manifest.read_text().splitlines())
    for n in range(520):
        doc = {"height": 1000 + n, "description": "single", "batch": n}
        assert upsert([doc], "blocks") == (200, {"ok": True, "upserted": 1})
        query = json.dumps({"table": "blocks", "field": "batch", "value": str(n)})
        status, reply = http_request(i, "POST", "/search", query, headers)
        assert status == 200 and [hit["height"] for hit in reply["hits"] if hit["description"] == "single"] == [1000 + n]
        if n == 254:
            assert len(manifest.read_text().splitlines()) == seeded_segments
    assert len(manifest.read_text().splitlines()) == seeded_segments + 2
    # Leave the last eight rows in the WAL and simulate an interrupted append.
    indexer.kill()
    indexer.wait(timeout=10)
    wal, = blocks_dir.glob("updates.*.wal")
    with wal.open("ab") as interrupted:
        interrupted.write(b'[{"height":99999')
    assert http_request(s, "POST", "/search", json.dumps({"field": "hash", "value": rows[0]["hash"]}),
                {"Content-Type": "application/json"})[0] == 502
    tx_wal, = (index / "transactions").glob("updates.*.wal")
    buffered_docs = sum(len(json.loads(line)) for line in tx_wal.read_text().splitlines())
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
    previous_count = len(manifest.read_text().splitlines())
    large = dict(changed, hash="large", value="x" * (2 * 1024 * 1024))
    assert upsert([large])[0] == 200
    assert len(manifest.read_text().splitlines()) == previous_count
    assert cli("searcher", "query", "hash", "large", s)["hits"] == [large]
    assert upsert([changed])[0] == 200
    assert len(manifest.read_text().splitlines()) == previous_count + 1
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
    analyzed = dict(blocks_table, name="analyzed", primary_key="description")
    schema.write_text(json.dumps({"tables": [transactions, blocks_table, empty, analyzed]}))
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
    # An analyzed primary key is still searchable by individual tokens.
    assert cli("searcher", "query", "analyzed", "description", "ethereum", s)["hits"][0]["height"] == block["height"]
    assert cli("searcher", "query", "blocks", "height", "016", s)["hits"] == []
    headers = {"Content-Type": "application/json"}
    assert http_request(b, "POST", "/update", '{"table":"events","documents":[{}]}', headers)[0] == 400
    def write_block(height):
        doc = dict(block, height=height, description="extra", metadata={"network":"test","tags":[]}, batch=9)
        return http_request(b, "POST", "/update", json.dumps({"table": "blocks", "documents": [doc]}), headers)
    block_manifest = index / "blocks" / "segments"
    segment_count = len(block_manifest.read_text().splitlines())
    with ThreadPoolExecutor(max_workers=4) as clients:
        writes = list(clients.map(write_block, range(100, 104)))
    assert writes == [(200, {"ok": True, "upserted": 1})] * 4, writes
    assert len(block_manifest.read_text().splitlines()) == segment_count + 1
    for h in range(100, 104):
        assert cli("searcher", "query", "blocks", "height", str(h), s)["hits"][0]["height"] == h
    indexer.terminate()
    indexer.wait(timeout=10)
    blocks.unlink()
    start(stack, "indexer", i, index, i)
    expected = {key: value for key, value in block.items() if key != "batch"}
    for field, term in (("metadata.tags", "offline"), ("metadata.network", "ethereum"),
                        ("description", "ethereum"), ("height", "16"), ("batch", "1")):
        assert cli("searcher", "query", "blocks", field, term, s)["hits"] == [expected]
    assert cli("searcher", "query", "events", "hash", "xyz", s)["hits"] == []
    assert cli("searcher", "query", "transactions", "blockNumber", "0x10", s)["hits"] == rows
    cli("searcher", "query", "transactions", "value", "0x0", s, ok=False)
    cli("searcher", "query", "missing_table", "height", "16", s, ok=False)

    invalid_key = dict(block, height="not-an-integer")
    assert http_request(b, "POST", "/update", json.dumps({"table": "blocks", "documents": [invalid_key]}), headers)[0] == 400
    updated = dict(block, height="16", description="Updated")
    update_reply = http_request(b, "POST", "/update", json.dumps({"table": "blocks", "documents": [updated]}), headers)
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
    catalog["tables"].append(dict(tx_schema, name="columns",
                                  fields=[dict(field, indexed=False) for field in tx_schema["fields"]]))
    schema_path.write_text(json.dumps(catalog))
    origin = endpoints()[0]
    server = start(stack, "indexer", origin, index, origin)
    first, second = json.loads(SAMPLE.read_text())

    def write(docs):
        payload = json.dumps({"table": "transactions", "documents": docs})
        reply = http_request(origin, "POST", "/update", payload, headers)
        assert reply[0] == 200, reply

    def hits(field, term):
        reply = http_request(origin, "POST", "/search", json.dumps({"field": field, "value": term}), headers)
        assert reply[0] == 200
        return reply[1]["hits"]

    write([dict(second, value="patched")])
    assert hits("hash", second["hash"]) == [dict(second, value="patched")]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "indexer", origin, index, origin)
    assert hits("from", second["from"]) == [first, dict(second, value="patched")]
    write([dict(first, value=None)])

    def latest_dictionary(table):
        directory = index / table
        last_id = int((directory / "segments").read_text().splitlines()[-1].split()[0])
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
    start(stack, "indexer", origin, index, origin)
    assert hits("from", first["from"]) == [dict(first, value="after-restart"), inserted]


with tempfile.TemporaryDirectory(dir="/tmp") as work:
    index = Path(work)
    for bad_value in (0, -1, 1.5, "256", None):
        bad_table = dict(transactions, flush_docs=bad_value)
        (index / "schema.json").write_text(json.dumps({"tables": [bad_table]}))
        failure = subprocess.run([BIN / "indexer", "build", index], capture_output=True, text=True)
        assert failure.returncode == 1 and "flush_docs" in failure.stderr
