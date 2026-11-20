"""Exercise candidate replacement, finalization and crash recovery over HTTP."""

import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path

from searcher_fixture import Searcher


def run(binary, index, stack):
    # Block 0 fills one disk segment. Block 1 is acknowledged through the WAL.
    searcher = Searcher(binary, index, stack)
    searcher.expect_height(-1)
    a = {"hash": "a", "from": "alice", "value": "10"}
    b = {"hash": "b", "from": "bob", "value": "20"}
    searcher.submit("/solidity", 0, [a, b])
    searcher.expect_height(0)
    searcher.expect_hits("(from:alice OR from:bob) AND hash:a", a)
    for expression in ("hash:(", "unknown:x", "value:10"):
        searcher.post("/search", {"query": expression}, 400)
    searcher.expect_hits("hash:a", a)

    first = {**a, "from": "fork-a"}
    orphan = {**first, "hash": "orphan"}
    searcher.submit("/candidate", 1, [first, orphan])
    searcher.expect_hits("from:fork-a", first, orphan)
    replacement = {**a, "from": "fork-b"}
    searcher.submit("/candidate", 1, [replacement])
    searcher.expect_hits("from:fork-a")
    searcher.expect_hits("hash:orphan")
    future = {**b, "from": "pending"}
    searcher.submit("/candidate", 2, [future])
    searcher.expect_hits("hash:a", replacement)
    searcher.expect_hits("hash:b", future)
    searcher.expect_hits("*:*", replacement, future)
    searcher.expect_hits("from:alice OR from:fork-a")
    searcher.expect_height(0)

    # A bad second row must roll back the first one and preserve both candidates.
    searcher.submit("/solidity", 1, [{**a, "hash": "partial"}, {"from": "invalid"}], 400)
    searcher.expect_hits("hash:partial")
    searcher.expect_hits("hash:a", replacement)
    searcher.expect_hits("hash:b", future)
    searcher.expect_height(0)

    confirmed = {**a, "value": "15"}
    searcher.submit("/solidity", 1, [confirmed])
    searcher.expect_height(1)
    searcher.expect_hits("hash:a", confirmed)
    searcher.expect_hits("from:fork-b")
    searcher.expect_hits("hash:b", future)

    searcher.crash_and_reopen()
    searcher.expect_height(1)
    searcher.expect_hits("hash:a", confirmed)
    searcher.expect_hits("hash:b", b)
    searcher.expect_hits("from:pending")
    # The next block changes an indexed field and checkpoints the recovered row.
    moved = {**confirmed, "from": "carol"}
    searcher.submit("/solidity", 2, [moved])
    searcher.crash_and_reopen()
    searcher.expect_height(2)
    searcher.expect_hits("from:alice")
    searcher.expect_hits("hash:a", moved)
    searcher.expect_hits("from:carol", moved)


def delayed_finalization(binary, index, stack):
    searcher = Searcher(binary, index, stack)
    rows = []
    for height in range(18):
        row = {"hash": f"tx{height}", "from": "alice", "value": str(height)}
        rows.append(row)
        searcher.submit("/candidate", height, [row])
        searcher.expect_height(max(-1, height - 17))
        searcher.expect_hits("*:*", *rows)
        if height >= 16:
            target = height - 16
            searcher.submit("/solidity", target, [rows[target]])
            searcher.expect_height(target)
            searcher.expect_hits("*:*", *rows)

    searcher.crash_and_reopen()
    searcher.expect_height(1)
    searcher.expect_hits("*:*", *rows[:2])


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    for scenario in (run, delayed_finalization):
        with tempfile.TemporaryDirectory() as work, ExitStack() as stack:
            scenario(binary, Path(work), stack)
