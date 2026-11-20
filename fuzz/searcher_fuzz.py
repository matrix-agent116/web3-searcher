"""Compare reproducible HTTP operation sequences against an in-memory table."""

import json
import os
import random
import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path

from searcher_fixture import Searcher


HASHES = ("tx0", "tx1", "tx2", "tx3", "tx4")
CHAINS = ("eth", "arb")
SENDERS = ("alice", "bob", "钱包")


class Model:
    def __init__(self, match_fields):
        self.match_fields = match_fields
        self.height = -1
        self.confirmed = {}
        self.candidates = {}

    def visible(self):
        result = self.confirmed.copy()
        for height in sorted(self.candidates):
            result.update(self.candidates[height])
        return result

    def submit(self, path, height, rows):
        if height <= self.height:
            return (200, 0) if path == "/candidate" else (400, 0)
        if any(row.get(field) is None for row in rows for field in self.match_fields):
            return 400, 0
        batch = {tuple(row[field] for field in self.match_fields): row for row in rows}
        if path == "/candidate":
            if batch:
                self.candidates[height] = batch
            else:
                self.candidates.pop(height, None)
        else:
            self.height = height
            self.confirmed.update(batch)
            self.candidates = {h: block for h, block in self.candidates.items() if h > height}
        return 200, len(rows)

    def check(self, searcher):
        searcher.expect_height(self.height)
        rows = self.visible().values()
        for field, terms in (("hash", HASHES), ("chain", CHAINS), ("from", SENDERS)):
            for term in terms:
                searcher.expect_hits(f"{field}:{term}", *(row for row in rows if row[field] == term))
        searcher.expect_hits("(chain:eth AND from:alice) OR hash:tx0",
                            *(row for row in rows if (row["chain"] == "eth" and row["from"] == "alice")
                              or row["hash"] == "tx0"))


def make_batch(rng, model):
    known = list(model.visible().values())
    batch = []
    for _ in range(rng.randrange(6)):
        # Reuse keys within and across blocks to exercise full and column-only updates.
        if batch and rng.randrange(4) == 0:
            row = rng.choice(batch).copy()
        elif known and rng.randrange(3) == 0:
            row = rng.choice(known).copy()
        else:
            row = {"hash": rng.choice(HASHES), "chain": rng.choice(CHAINS),
                   "from": rng.choice(SENDERS)}
        row["value"] = rng.choice([None, "", 'quoted "value"\n', str(rng.randrange(10000))])
        batch.append(row)
    return batch


def next_operation(rng, model):
    if rng.randrange(16) == 0:
        return "restart", None, []
    path = rng.choice(("/candidate", "/solidity"))
    height = model.height + rng.randint(1, 3)
    if model.candidates and rng.randrange(2) == 0:
        height = rng.choice(sorted(model.candidates))
    if model.height >= 0 and rng.randrange(9) == 0:
        height = rng.randrange(model.height + 1)
    rows = make_batch(rng, model)
    if rng.randrange(8) == 0:
        # Put a malformed row last so a partial batch commit becomes observable.
        rows.append({"from": rng.choice(SENDERS), "value": "missing key"})
    return path, height, rows


def operations(rng, model, steps):
    # Keep the upstream chain across searcher restarts, including empty blocks.
    pending = {}
    restart_at = rng.randint(16, 20)
    for height in range(restart_at + 2):
        pending[height] = make_batch(rng, model)
        yield "/candidate", height, pending[height]
        if rng.randrange(3) == 0:
            fork = rng.choice(list(pending))
            pending[fork] = make_batch(rng, model)
            yield "/candidate", fork, pending[fork]
        if height == restart_at:
            yield "restart", None, []
        if height >= 16:
            target = height - 16
            yield "/solidity", target, pending.pop(target)

    for _ in range(steps):
        yield next_operation(rng, model)
    yield "restart", None, []


def run(binary, seed, steps):
    rng = random.Random(seed)
    fields = ("hash",) if seed % 2 == 0 else ("hash", "chain")
    model = Model(fields)
    print(f"searcher_fuzz seed={seed} steps={steps} match_fields={fields}", flush=True)
    with tempfile.TemporaryDirectory() as work, ExitStack() as stack:
        searcher = Searcher(binary, Path(work), stack, indexed=("hash", "chain", "from"),
                          flush_docs=5, merge_segments=3)
        model.check(searcher)
        for step, (path, height, rows) in enumerate(operations(rng, model, steps), start=1):
            try:
                if path == "restart":
                    searcher.crash_and_reopen()
                    model.candidates.clear()
                else:
                    status, accepted = model.submit(path, height, rows)
                    searcher.submit(path, height, rows, status, match_fields=fields, accepted=accepted)
                model.check(searcher)
            except Exception as error:
                raise AssertionError(
                    f"seed={seed} steps={steps} step={step} operation={path} "
                    f"height={height} rows={json.dumps(rows, ensure_ascii=False)}\n"
                    f"Replay: SEARCHER_FUZZ_SEED={seed} SEARCHER_FUZZ_STEPS={steps} "
                    "ctest --test-dir build -R '^searcher_e2e_fuzz$' --output-on-failure"
                ) from error


if __name__ == "__main__":
    binary = Path(sys.argv[1]).resolve()
    requested_seed = os.environ.get("SEARCHER_FUZZ_SEED")
    if requested_seed is None:
        seeds = (7, 42, 113)
    elif requested_seed == "random":
        seeds = (random.SystemRandom().getrandbits(64),)
    else:
        seeds = (int(requested_seed, 0),)
    steps = int(os.environ.get("SEARCHER_FUZZ_STEPS", "64"))
    if steps < 1:
        raise ValueError("SEARCHER_FUZZ_STEPS must be positive")
    for seed in seeds:
        run(binary, seed, steps)
