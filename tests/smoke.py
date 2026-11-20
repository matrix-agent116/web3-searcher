"""Exercise all three executables and the on-disk index via their public APIs."""

import fcntl
import json
from itertools import count
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, TimeoutError
from contextlib import ExitStack

from service_process import endpoints, http_request, start_service

BIN = Path(sys.argv[1]).resolve()
EXAMPLES = Path(__file__).resolve().parents[1] / "examples"
SAMPLE = EXAMPLES / "searcher" / "transactions.json"
ROUTER_CONFIG = EXAMPLES / "searcher" / "router.conf"


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
    return start_service(stack, BIN / name, endpoint, *arguments)


def cli(binary, *args, ok=True):
    run = subprocess.run([BIN / binary, *args], capture_output=True,
                         text=True, timeout=30)
    assert run.returncode == (0 if ok else 1), (args, run.stdout, run.stderr)
    result = json.loads(run.stdout)
    assert result["ok"] == ok, result
    return result


def index_query(origin, expression, *, table="transactions", ok=True, limit=None):
    body = {"table": table, "query": expression}
    if limit is not None:
        body["limit"] = limit
    return cli("searcher", "query", "/search", json.dumps(body), origin, ok=ok)


def public_height(origin):
    status, result = http_request(origin, "GET", "/v1/chain/height")
    assert status == 200, result
    return result["block_number"]


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
    start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    # The only generated row has a null recipient. Both aggregate routes must
    # accept an empty match, even when more readers than aggregate slots run.
    for mode in ("hash", "received_stats", "top_senders"):
        single_kind = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block", "1",
                          "--query-mode", mode, "--queries", "9", "--query-threads", "6")
        assert single_kind["search"]["requests"] == single_kind["query_types"][mode]["requests"] == 9
        assert all(value is None for name, value in single_kind["query_types"].items() if name != mode)
    hashes = set()
    warmup = cli("indexer_benchmark", origin, "--blocks", "16", "--tx-per-block", "3", "--queries", "5")
    assert warmup["candidate"]["requests"] == 16 and warmup["solidity"] is None
    assert warmup["transactions"] == 0 and warmup["search"]["requests"] == 5
    assert {name: result["requests"] for name, result in warmup["query_types"].items()} == {
        "hash": 2, "received_stats": 2, "top_senders": 1}
    assert warmup["last_solidity_block"] == block_number(origin, "transactions") == -1
    for first, block_count, finalized in ((0, 18, 1), (18, 2, 3)):
        stats = cli("indexer_benchmark", origin, "--blocks", str(block_count), "--tx-per-block",
                    "3", "--queries", "7", "--query-threads", "4")
        assert (stats["first_block"], stats["last_block"]) == (first, first + block_count - 1)
        assert stats["transactions"] == 6 and stats["solidity"]["requests"] == 2
        assert stats["candidate"]["requests"] == block_count
        assert stats["transactions_per_second"] > 0 and stats["search"]["requests"] == 7
        assert [stats["query_types"][name]["requests"] for name in
                ("hash", "received_stats", "top_senders")] == [3, 2, 2]
        for phase in (stats["candidate"], stats["solidity"], stats["search"], *stats["query_types"].values()):
            assert phase["requests_per_second"] > 0
            assert 0 < phase["p50_ms"] <= phase["p95_ms"] <= phase["p99_ms"]
            assert phase["max_ms"] > 0 and 0 <= phase["over_1s"] <= phase["requests"]
        assert stats["last_solidity_block"] == block_number(origin, "transactions") == finalized
        for height in (first, first + 1):
            found = index_query(origin, f"blockNumber:{hex(height)}")
            assert len(found["hits"]) == 3
            for row in found["hits"]:
                assert set(row) == {"hash", "from", "to", "blockNumber", "value"}
                for field, digits in (("hash", 64), ("from", 40), ("to", 40)):
                    if field == "to" and row[field] is None:
                        continue
                    assert row[field].startswith("0x") and len(row[field]) == digits + 2
                    int(row[field][2:], 16)
                assert row["blockNumber"] == hex(height) and row["value"].isascii() and row["value"].isdecimal()
                hashes.add(row["hash"])
    assert len(hashes) == 12
    writes_only = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block",
                      "1", "--queries", "0")
    assert writes_only["last_block"] == 20 and writes_only["search"] is None
    assert all(phase is None for phase in writes_only["query_types"].values())
    # A single completed write step ends continuous mode immediately.
    # An explicit query budget still runs in full.
    single = cli("indexer_benchmark", origin, "--blocks", "1", "--tx-per-block", "1")
    assert single["last_block"] == 21 and single["search"] is None
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
    assert http_request(origin, "POST", "/solidity", json.dumps({
        "table": "transactions", "block_number": height, "match_field": ["hash"],
        "documents": []}), {"Content-Type": "application/json"})[0] == 200
    overflow = subprocess.run([BIN / "indexer_benchmark", origin, "--blocks", "2"],
                              capture_output=True, timeout=10)
    assert overflow.returncode != 0 and block_number(origin, "transactions") == height


with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    root = Path(work)
    index = root / "index"
    i, b = endpoints(2)
    prepare_catalog(index)
    schema_path = index / "schema.json"
    catalog = json.loads(schema_path.read_text())
    transactions = catalog["tables"][0]
    transactions["fields"].append({"name": "payload", "type": "VARCHAR", "stored": True, "indexed": False})
    # Keep the rollback and recovery checkpoints in this scenario small.
    for table in catalog["tables"]:
        table["flush_docs"] = 256
    schema_path.write_text(json.dumps(catalog))
    server = start(stack, "searcher", i, index, i, ROUTER_CONFIG)
    start(stack, "updater", b, b, i)
    assert http_request(i, "GET", "/search")[0] == 405
    assert http_request(i, "POST", "/build")[0] == 404
    assert http_request(i, "POST", "/search", "{}")[0] == 415
    assert http_request(i, "POST", "/search", "{", {"Content-Type": "application/json"})[0] == 400
    assert http_request(i, "POST", "/index", "{}", {"Content-Type": "application/json"})[0] == 404
    for endpoint in (i, b):
        for removed in ("/update", "/upsert"):
            assert http_request(endpoint, "POST", removed, "{}", {"Content-Type": "application/json"})[0] == 404
    rows = [dict(row, payload=None) for row in json.loads(SAMPLE.read_text())]
    address = rows[0]["to"]
    wei = rows[0]["value"]
    received = {"transfer_count": 1, "value_count": 1, "total_wei": wei,
                "average_wei": wei, "min_wei": wei, "max_wei": wei}
    stats_path = "/v1/account/received-stats"
    expected = (200, {"ok": True, "hits": [received]})
    assert http_request(i, "GET", f"{stats_path}?address={address}") == expected
    assert http_request(i, "POST", stats_path, json.dumps({"address": address}),
                        {"Content-Type": "application/json"}) == expected
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
        assert index_query(i, f"{field}:{term}")["hits"] == expected
    both = f"(from:{rows[0]['from']} OR to:{rows[0]['to']}) AND blockNumber:0x10"
    assert index_query(i, both)["hits"] == rows
    for invalid in ("unknown:x", "to:(", "unqualified", "", 7, None):
        status, reply = http_request(i, "POST", "/search", json.dumps({"query": invalid}),
                                     {"Content-Type": "application/json"})
        assert status == 400 and reply["error"], (invalid, status, reply)
    request = [b'{"query":"block', b'Number:0x10"}']
    assert http_request(i, "POST", "/search", iter(request), {"Content-Type": "application/json"},
                chunked=True) == (200, {"ok": True, "hits": rows})
    headers = {"Content-Type": "application/json"}
    next_height = count(2)
    def upsert(docs, table="transactions"):
        height = next(next_height)
        reply = http_request(b, "POST", "/solidity", json.dumps({
            "table": table, "block_number": height,
            "match_field": ["height" if table == "blocks" else "hash"], "documents": docs}), headers)
        if reply[0] == 200:
            assert reply[1] == {"ok": True, "block_number": height, "upserted": len(docs)}
        return reply

    changed = dict(rows[0], blockNumber="0x20", value="123")
    new = dict(rows[1], hash="new-transaction", blockNumber="0x30")
    batch = [dict(changed, blockNumber="0x19"), new, changed]
    source = root / "updates.json"
    source.write_text(json.dumps(batch))
    assert cli("updater", "solidity", "transactions", "1", '["hash"]', source, b) == {"ok": True, "block_number": 1, "upserted": 3}
    assert upsert(batch)[0] == 200
    assert index_query(i, "blockNumber:0x10")["hits"] == rows[1:]
    assert index_query(i, "blockNumber:0x19")["hits"] == []
    assert index_query(i, f"hash:{rows[0]['hash']}")["hits"] == [changed]
    assert index_query(i, f"hash:{new['hash']}")["hits"] == [new]
    # Hold the same directory lock a second searcher would use. While the
    # writer is waiting, another worker must still serve the committed row.
    workers = len(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else os.cpu_count()
    if workers and workers > 1:
        replacement = dict(changed, value="124")
        query = json.dumps({"query": f"hash:{changed['hash']}"})
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
                                        json.dumps({"query": "blockNumber:snapshot"}), headers)
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
        assert upsert([dict(changed, value="999")] * 256)[0] >= 400
        assert (table_path / "segments").read_bytes() == published
        assert set(table_path.iterdir()) == files_before | {pending}
        assert index_query(i, f"hash:{rows[0]['hash']}")["hits"] == [changed]
    finally:
        pending.rmdir()
    assert upsert([changed])[0] == 200
    assert upsert([dict(changed, value="999"), {"hash": None}])[0] == 400
    assert upsert([{}])[0] == 400
    assert upsert([changed], "missing")[0] == 400
    assert index_query(i, f"hash:{rows[0]['hash']}")["hits"] == [changed]
    # Sequential callers see every acknowledged single-row write. A checkpoint
    # packs hundreds of such requests, also preserving indexed-only fields.
    blocks_dir = index / "blocks"
    manifest = blocks_dir / "segments"
    seeded_segments = len(segment_entries(manifest))
    for n in range(520):
        doc = {"height": 1000 + n, "description": "single", "batch": n}
        assert upsert([doc], "blocks")[0] == 200
        query = json.dumps({"table": "blocks", "query": f"batch:{n}"})
        status, reply = http_request(i, "POST", "/search", query, headers)
        assert status == 200 and [hit["height"] for hit in reply["hits"] if hit["description"] == "single"] == [1000 + n]
        if n == 254:
            assert len(segment_entries(manifest)) == seeded_segments
    assert len(segment_entries(manifest)) == seeded_segments + 2
    # Leave the last eight rows in the WAL and simulate an interrupted append.
    server.kill()
    server.wait(timeout=10)
    wal, = blocks_dir.glob("updates.*.wal")
    with wal.open("ab") as interrupted:
        interrupted.write(b'[{"height":99999')
    try:
        http_request(i, "GET", "/health")
    except ConnectionRefusedError:
        pass
    else:
        raise AssertionError("searcher listener survived process termination")
    tx_wal, = (index / "transactions").glob("updates.*.wal")
    buffered_docs = sum(len(json.loads(line)["documents"]) for line in tx_wal.read_text().splitlines())
    transactions["flush_docs"] = buffered_docs + 2
    schema_path.write_text(json.dumps(catalog))
    start(stack, "searcher", i, index, i, ROUTER_CONFIG)
    for n in (0, 255, 256, 511, 512, 519):
        assert index_query(i, f"height:{1000 + n}", table="blocks")["hits"][0]["height"] == 1000 + n
    assert index_query(i, "height:99999", table="blocks")["hits"] == []
    assert upsert([{"height": 99999}], "blocks")[0] == 200
    # Cover both the durable and buffered versions of one key at the next
    # checkpoint, then overwrite it again in memory.
    assert upsert([{"height": 1000, "description": "intermediate"}], "blocks")[0] == 200
    final = {"height": 1000, "description": "checkpointed"}
    assert upsert([final] * 256, "blocks")[0] == 200
    assert index_query(i, "description:intermediate", table="blocks")["hits"] == []
    assert index_query(i, "description:checkpointed", table="blocks")["hits"][0]["height"] == 1000
    assert upsert([dict(final, description="latest")], "blocks")[0] == 200
    assert index_query(i, "description:checkpointed", table="blocks")["hits"] == []
    hits = index_query(i, "description:single", table="blocks", limit=520)["hits"]
    assert [hit["height"] for hit in hits] == list(range(1001, 1520))
    assert index_query(i, f"hash:{rows[0]['hash']}")["hits"] == [changed]
    assert index_query(i, f"hash:{new['hash']}")["hits"] == [new]
    assert index_query(i, "blockNumber:0x10")["hits"] == rows[1:]

    # A large row remains buffered. The next row reaches the configured count.
    manifest = index / "transactions" / "segments"
    previous_count = len(segment_entries(manifest))
    large = dict(changed, hash="large", payload="x" * (2 * 1024 * 1024))
    assert upsert([large])[0] == 200
    assert len(segment_entries(manifest)) == previous_count
    assert index_query(i, "hash:large")["hits"] == [large]
    assert upsert([changed])[0] == 200
    assert len(segment_entries(manifest)) == previous_count + 1
    assert index_query(i, "hash:large")["hits"] == [large]
    restored = index_query(i, f"hash:{changed['hash']}")["hits"]
    assert restored == [changed], (restored, changed)

with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    root = Path(work)
    index = root / "catalog"
    i, b = endpoints(2)
    prepare_catalog(index)
    schema = index / "schema.json"
    config = json.loads(schema.read_text())
    transactions, blocks_table = config["tables"]
    schema.write_text(json.dumps({"tables": [transactions]}))
    assert cli("searcher", "build", index) == {"ok": True, "tables": 1, "indexed": 2}
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
    failed = subprocess.run([BIN / "searcher", "serve", index, i], capture_output=True, timeout=10)
    assert failed.returncode == 1
    assert list((index / "blocks").iterdir()) == []
    assert not list(index.glob(".table.*"))
    blocks.write_text(json.dumps([block]))
    # Startup opens transactions, fills the empty blocks directory and creates events.
    start(stack, "updater", b, b, i)
    (root / "blocks_by_tag.json").write_text(json.dumps({
        "path": "/v1/blocks/by-tag", "table": "blocks", "methods": ["POST"],
        "query": {"lucene": "metadata.tags:${tag}",
                          "select": {name: name for name in ("height", "description", "metadata")}}}))
    (root / "router.conf").write_text("blocks_by_tag.json\n")
    server = start(stack, "searcher", i, index, i, root / "router.conf")
    assert all((index / name / "segments").is_file() for name in ("transactions", "blocks", "events"))
    assert (old_manifest.stat().st_ino, old_manifest.stat().st_mtime_ns) == (old_stat.st_ino, old_stat.st_mtime_ns)
    # An analyzed match field is still searchable by individual tokens.
    assert index_query(i, "description:ethereum", table="analyzed")["hits"][0]["height"] == block["height"]
    phrase = 'description:"SAMPLE ethereum" AND metadata.tags:off*'
    assert index_query(i, phrase, table="blocks")["hits"][0]["height"] == block["height"]
    assert index_query(i, 'description:"ethereum sample"', table="blocks")["hits"] == []
    # Separate range clauses can match different terms of the same document.
    for field, low, high in (("metadata.tags", "offline", "demo"),
                             ("description", "sample", "block")):
        expression = f"{field}:[{low} TO *] AND {field}:[* TO {high}]"
        assert [row["height"] for row in index_query(i, expression, table="blocks")["hits"]] == [16]
    assert index_query(i, "height:016", table="blocks")["hits"] == []
    headers = {"Content-Type": "application/json"}
    assert http_request(b, "POST", "/solidity", '{"table":"events","documents":[{}]}', headers)[0] == 400
    keyed_batch = [dict(block, description=word) for word in ("overwritten", "current")]
    assert http_request(b, "POST", "/solidity", json.dumps({"table": "indexed_only", "block_number": 1, "match_field": ["height"], "documents": keyed_batch}), headers)[0] == 200
    assert len(index_query(i, f"height:{block['height']}", table="indexed_only")["hits"]) == 1
    def check_indexed_only():
        assert index_query(i, "description:overwritten", table="indexed_only")["hits"] == []
        assert index_query(i, "description:ethereum", table="indexed_only")["hits"] == []
        hits = index_query(i, "description:current", table="indexed_only")["hits"]
        assert len(hits) == 1 and "height" not in hits[0]

    check_indexed_only()
    analyzed_docs = [dict(block, description="common alpha", height=11),
                     dict(block, description="common beta", height=12),
                     dict(block, description="common alpha", height=13)]
    assert http_request(b, "POST", "/solidity", json.dumps({"table": "analyzed", "block_number": 1, "match_field": ["description"], "documents": analyzed_docs}), headers)[0] == 200
    assert sorted(row["height"] for row in index_query(i, "description:common", table="analyzed")["hits"]) == [12, 13]
    def write_block(height, number=1):
        doc = dict(block, height=height, description="extra", metadata={"network":"test","tags":[]}, batch=9)
        return http_request(b, "POST", "/solidity", json.dumps({"table": "blocks", "block_number": number, "match_field": ["height"], "documents": [doc]}), headers)
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
        assert index_query(i, f"height:{h}", table="blocks")["hits"][0]["height"] == h
    server.terminate()
    server.wait(timeout=10)
    blocks.unlink()
    start(stack, "searcher", i, index, i, root / "router.conf")
    check_indexed_only()
    assert index_query(i, "description:alpha", table="analyzed")["hits"][0]["height"] == 13
    expected = {key: value for key, value in block.items() if key != "batch"}
    for field, term in (("metadata.tags", "offline"), ("metadata.network", "ethereum"),
                        ("description", "ethereum"), ("height", "16"), ("batch", "1")):
        assert index_query(i, f"{field}:{term}", table="blocks")["hits"] == [expected]
    assert index_query(i, "hash:xyz", table="events")["hits"] == []
    assert index_query(i, "blockNumber:0x10")["hits"] == json.loads(SAMPLE.read_text())
    index_query(i, "value:0", ok=False)
    index_query(i, "height:16", table="missing_table", ok=False)

    invalid_key = dict(block, height="not-an-integer")
    assert http_request(b, "POST", "/solidity", json.dumps({"table": "blocks", "block_number": 5, "match_field": ["height"], "documents": [invalid_key]}), headers)[0] == 400
    updated = dict(block, height="16", description="Updated")
    update_reply = http_request(b, "POST", "/solidity", json.dumps({"table": "blocks", "block_number": 5, "match_field": ["height"], "documents": [updated]}), headers)
    assert update_reply[0] == 200, update_reply
    assert index_query(i, "description:ethereum", table="blocks")["hits"] == []
    expected = dict(expected, description="Updated")

    def search_blocks(value):
        return http_request(i, "POST", "/v1/blocks/by-tag", json.dumps({"tag": value}), headers)

    terms = ["offline", "missing"] * 8
    with ThreadPoolExecutor(max_workers=8) as clients:
        for term, response in zip(terms, clients.map(search_blocks, terms)):
            assert response == (200, {"ok": True, "hits": [expected] if term == "offline" else []})

    damaged = root / "damaged"
    prepare_catalog(damaged)
    with ThreadPoolExecutor(max_workers=2) as clients:
        results = list(clients.map(lambda _: cli("searcher", "build", damaged), range(2)))
    # Each table is created atomically. The two builders can seed different tables.
    assert all(report["tables"] == 2 for report in results), results
    assert sum(report["indexed"] for report in results) == 3, results
    assert cli("searcher", "build", damaged)["indexed"] == 0
    with ExitStack() as reader:
        origin, = endpoints(1)
        start(reader, "searcher", origin, damaged, origin)
        assert index_query(origin, "*:*")["hits"] == json.loads(SAMPLE.read_text())
        seed_block, = json.loads(SAMPLE.with_name("blocks.json").read_text())
        del seed_block["batch"]  # indexed-only field
        assert index_query(origin, "*:*", table="blocks")["hits"] == [seed_block]
    broken_manifest = damaged / "transactions" / "segments"
    broken_manifest.write_text("corrupt")
    failure = subprocess.run([BIN / "searcher", "build", damaged], capture_output=True, timeout=10)
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
    server = start(stack, "searcher", origin, index, origin)
    first, second = json.loads(SAMPLE.read_text())

    next_height = count(1)
    def write(docs, match_field="hash"):
        payload = json.dumps({"table": "transactions", "block_number": next(next_height), "match_field": [match_field], "documents": docs})
        reply = http_request(origin, "POST", "/solidity", payload, headers)
        assert reply[0] == 200, reply

    def hits(field, term):
        reply = http_request(origin, "POST", "/search", json.dumps({"query": f"{field}:{term}"}), headers)
        assert reply[0] == 200
        return reply[1]["hits"]

    # Reject a nonunique match atomically, including prior rows in the request.
    for field in ("from", "value", "unknown"):
        reply = http_request(origin, "POST", "/solidity", json.dumps({
            "table": "transactions", "block_number": 1, "match_field": [field],
            "documents": [dict(first, hash="uncommitted", **{"from": "elsewhere"}), first]
        }), headers)
        assert reply[0] == 400, reply
        assert hits("hash", "uncommitted") == []

    write([dict(second, value="2")])
    assert hits("hash", second["hash"]) == [dict(second, value="2")]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("from", second["from"]) == [first, dict(second, value="2")]
    write([dict(first, value=None)])

    def latest_dictionary(table):
        directory = index / table
        last_id = int(segment_entries(directory / "segments")[-1].split()[0])
        return (directory / f"{last_id:016x}.idx").read_bytes()

    empty_dictionary = latest_dictionary("columns")
    assert latest_dictionary("transactions") == empty_dictionary
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("from", first["from"]) == [dict(first, value=None), dict(second, value="2")]

    moved = dict(second, **{"from": "new-address", "value": "3"})
    latest = dict(moved, value="4")
    write([moved, latest])
    assert latest_dictionary("transactions") != empty_dictionary
    assert hits("from", first["from"]) == [dict(first, value=None)]
    assert hits("from", "new-address") == [latest]
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("from", "new-address") == [latest]
    # Whole-row replacement clears omitted indexed fields.
    write([{"hash": second["hash"], "value": "5"}])
    assert hits("from", "new-address") == []
    # A patch followed by a full row in a mixed segment also survives replay.
    inserted = dict(first, hash="next-key")
    write([dict(first, value="6"), inserted])
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("from", first["from"]) == [dict(first, value="6"), inserted]

    # Change the match field across requests in one checkpoint. The old hash
    # must stay absent on replay even when a later request reuses that hash.
    other = dict(latest, hash="renamed", **{"from": "unique-address", "to": "unique-dest"})
    write([other])
    renamed = dict(other, hash="renamed-again")
    write([renamed], "to")
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("hash", "renamed") == []
    assert hits("hash", "renamed-again") == [renamed]
    patched = dict(renamed, value="7")
    write([patched], "from")
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin)
    assert hits("hash", "renamed-again") == [patched]
    different = dict(other, **{"from": "other-address", "to": "other-dest"})
    write([different])
    server.kill()
    server.wait(timeout=10)
    start(stack, "searcher", origin, index, origin)
    assert hits("hash", "renamed") == [different]
    assert hits("hash", "renamed-again") == [patched]


with tempfile.TemporaryDirectory(dir="/tmp") as work:
    index = Path(work)
    for bad_value in (0, -1, 1.5, "256", None):
        bad_table = dict(transactions, flush_docs=bad_value)
        (index / "schema.json").write_text(json.dumps({"tables": [bad_table]}))
        failure = subprocess.run([BIN / "searcher", "build", index], capture_output=True, text=True)
        assert failure.returncode == 1 and "flush_docs" in failure.stderr

    for bad_value in (0, 1, -1, 2.5, "3", None):
        bad_table = dict(transactions, merge_segments=bad_value)
        (index / "schema.json").write_text(json.dumps({"tables": [bad_table]}))
        failure = subprocess.run([BIN / "searcher", "build", index], capture_output=True, text=True)
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
    server = start(stack, "searcher", origin, index, origin)
    directory = index / "transactions"
    manifest = directory / "segments"
    first, second = json.loads(SAMPLE.read_text())

    def update(docs, field="hash", *, height):
        payload = {"table": "transactions", "block_number": height, "match_field": [field], "documents": docs}
        reply = http_request(origin, "POST", "/solidity", json.dumps(payload), headers)
        assert reply == (200, {"ok": True, "block_number": height, "upserted": len(docs)}), reply

    def query(field, value):
        payload = {"table": "transactions", "query": f"{field}:{value}"}
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
    patched = dict(first, value="8")
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
    server = start(stack, "searcher", origin, index, origin)
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
    start(stack, "searcher", origin, index, origin)
    for n in range(24):
        assert query("hash", f"concurrent-{n}") == [dict(added, hash=f"concurrent-{n}")]

# Tables use independent default or explicitly configured flush thresholds.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work)
    schema = json.loads(SAMPLE.with_name("schema.json").read_text())["tables"][0]
    schema.pop("documents")
    tables = [dict(schema, name=name, flush_docs=limit, merge_segments=2)
              for name, limit in (("fast", 3), ("slow", 17))]
    tables.append({"name": "default_limit", "fields": schema["fields"]})
    (index / "schema.json").write_text(json.dumps({"tables": tables}))
    origin = endpoints()[0]
    server = start(stack, "searcher", origin, index, origin)

    next_height = count(1)
    amounts = {"fast": "1", "slow": "2", "default_limit": "3"}
    def batch(table, hashes):
        height = next(next_height)
        rows = [{"hash": key, "value": amounts[table]} for key in hashes]
        status, result = http_request(origin, "POST", "/solidity", json.dumps({
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
    start(stack, "searcher", origin, index, origin)
    for table in ("fast", "slow"):
        status, result = http_request(origin, "POST", "/search", json.dumps({
            "table": table, "query": "hash:shared"}), headers)
        assert status == 200 and [row["value"] for row in result["hits"]] == [amounts[table]]
    batch("slow", ["shared", "last"])
    assert len(segment_entries(index / "slow" / "segments")) == 1
    assert not list((index / "slow").glob("updates.*.wal"))

    default_dir = index / "default_limit"
    batch("default_limit", (str(n) for n in range(14_399)))
    assert segment_entries(default_dir / "segments") == []
    assert list(default_dir.glob("updates.*.wal"))
    batch("default_limit", ["last"])
    assert len(segment_entries(default_dir / "segments")) == 1
    assert not list(default_dir.glob("updates.*.wal"))

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
    server = start(stack, "searcher", origin, index, origin)

    next_height = count(1)
    def submit(fields, docs, status=200):
        reply = http_request(origin, "POST", "/solidity", json.dumps({
            "table": "logs", "block_number": next(next_height), "match_field": fields, "documents": docs}), headers)
        assert reply[0] == status, (fields, reply)
        if status == 200:
            assert reply[1]["upserted"] == len(docs)

    def values(field, term):
        status, result = http_request(origin, "POST", "/search", json.dumps({
            "table": "logs", "query": f"{field}:{term}"}), headers)
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
    server = start(stack, "searcher", origin, index, origin)
    assert values("tag", "first") == ["patched"]

    changed = dict(a, tag="renamed", value="moved")
    submit(key, [changed, dict(b, chain=3, tag="fourth", value="four"), dict(changed, value="latest")])
    assert not wal.exists()
    assert values("hash", "A") == ["four", "latest", "two"]
    assert values("tag", "first") == []
    server.kill()
    server.wait(timeout=10)
    start(stack, "searcher", origin, index, origin)
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
    origin, updater = endpoints(2)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    start(stack, "updater", updater, updater, origin)
    directory = index / "transactions"

    def write(height, docs, expected_status=200):
        body = {"table": "transactions", "block_number": height,
                "match_field": ["hash"], "documents": docs}
        reply = http_request(updater, "POST", "/solidity", json.dumps(body), headers)
        assert reply[0] == expected_status, reply
        if expected_status == 200:
            assert reply[1] == {"ok": True, "upserted": len(docs), "block_number": height}

    assert public_height(origin) == -1
    assert (directory / "segments").read_text().splitlines()[0] == "block_number -1"
    write(-1, [], 400)
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert block_number(origin, "transactions") == -1
    write(0, [])
    assert public_height(origin) == 0
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert block_number(origin, "transactions") == 0
    write(0, [{"hash": "duplicate"}], 400)
    write(1, [{"hash": "first"}, {"hash": "second"}])
    assert (directory / "segments").read_text().splitlines()[0] == "block_number 1"
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert public_height(origin) == 1
    write(100, [{"hash": "a"}, {"hash": "b"}])
    assert not list(directory.glob("updates.*.wal"))
    write(101, [{"hash": "c", "value": "101"}])
    assert public_height(origin) == 101
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
    write(102, [{"value": "1"}], 400)
    write(99, [{"hash": "stale"}], 400)
    write(-1, [], 400)
    write("101", [], 400)
    write(None, [], 400)
    assert http_request(updater, "POST", "/solidity", json.dumps({
        "table": "transactions", "match_field": ["hash"], "documents": []}), headers)[0] == 400
    assert block_number(origin, "transactions") == 101

    server.kill()
    server.wait(timeout=10)
    with wal.open("ab") as truncated:
        truncated.write(b'{"block_number":999,"match_field":["hash"],"documents":[')
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert block_number(origin, "transactions") == 101
    assert cli("searcher", "query", "/v1/transactions/by-hash", '{"id":"c"}', origin)["hits"][0]["value"] == "101"
    assert wal.read_bytes().endswith(b"\n")
    write(101, [{"hash": "c", "value": "2"}], 400)
    write(102, [{"hash": "c", "value": "102"}])
    assert not wal.exists()
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert block_number(origin, "transactions") == 102

    write(105, [])
    assert public_height(origin) == 105
    server.kill()
    server.wait(timeout=10)
    server = start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert public_height(origin) == 105
    assert cli("searcher", "query", "/v1/transactions/by-hash", '{"id":"c"}', origin)["hits"][0]["value"] == "102"
    max_height = (1 << 64) - 1
    write(max_height, [{"hash": "x"}, {"hash": "y"}])
    assert public_height(origin) == max_height
    server.kill()
    server.wait(timeout=10)
    start(stack, "searcher", origin, index, origin, ROUTER_CONFIG)
    assert block_number(origin, "transactions") == max_height

# A table publishes each full candidate or finalized block in one transition.
with tempfile.TemporaryDirectory(dir="/tmp") as work, ExitStack() as stack:
    index = Path(work)
    schema = json.loads(SAMPLE.with_name("schema.json").read_text())["tables"][0]
    schema.pop("documents")
    (index / "schema.json").write_text(json.dumps({"tables": [schema]}))
    i, u = endpoints(2)
    server = start(stack, "searcher", i, index, i, ROUTER_CONFIG)
    start(stack, "updater", u, u, i)
    hdr = {"Content-Type": "application/json"}

    def send(path, number, docs):
        return http_request(u, "POST", path, json.dumps({"table": "transactions",
                            "block_number": number, "match_field": ["hash"],
                            "documents": docs}), hdr)

    def hashes():
        status, reply = http_request(i, "POST", "/v1/account/transfers", json.dumps(
            {"address": "sender"}), hdr)
        assert status == 200
        return sorted(row["transaction_id"] for row in reply["hits"])

    def docs(*keys):
        return [{"hash": key, "from": "sender"} for key in keys]

    assert send("/candidate", 5, docs("later"))[0] == 200
    assert send("/candidate", 3, docs("a", "b"))[0] == 200
    assert hashes() == ["a", "b", "later"]
    assert send("/candidate", 3, docs("x", "y"))[0] == 200
    assert hashes() == ["later", "x", "y"]
    assert send("/candidate", 3, docs("invalid") + [{}])[0] == 400
    assert send("/solidity", 3, docs("invalid") + [{}])[0] == 400
    assert public_height(i) == -1
    assert hashes() == ["later", "x", "y"]
    if workers and workers > 1:
        with ThreadPoolExecutor(max_workers=1) as clients:
            fd = os.open(index / "transactions", os.O_RDONLY)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX)
                confirmation = clients.submit(send, "/solidity", 3, docs("confirmed"))
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
        assert send("/solidity", 3, docs("confirmed"))[0] == 200
    assert public_height(i) == 3
    assert hashes() == ["confirmed", "later"]
    assert send("/solidity", 3, docs("second-part"))[0] == 400
    assert send("/candidate", 3, docs("late"))[1]["upserted"] == 0
    assert hashes() == ["confirmed", "later"]
    server.kill()
    server.wait(timeout=10)
    start(stack, "searcher", i, index, i, ROUTER_CONFIG)
    assert hashes() == ["confirmed"]
    assert public_height(i) == 3
