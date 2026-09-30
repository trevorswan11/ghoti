"""Mutation fuzzing for a time budget: python smoke.py [minutes] (default 20)."""

import random
import sys
import time

import corpus
import harness

budget = float(sys.argv[1]) * 60 if len(sys.argv) > 1 else 20 * 60
start = time.time()
seed = random.randrange(1000, 1000000)
total = 0
groups = {}
while time.time() - start < budget:
    cases = [(f"s{seed}_{n}", s) for n, s in corpus.mutated_cases(seed, 3)]
    random.shuffle(cases)
    for i in range(0, len(cases), 64):
        if time.time() - start >= budget:
            break
        batch = cases[i : i + 64]
        total += len(batch)
        for n, c, t in harness.run_many(batch):
            groups.setdefault(harness.signature(c, t), []).append((n, c))
    seed += 1
harness.report(groups)
print(f"{len(groups)} bad groups, {total} cases, {(time.time() - start) / 60:.1f} min")
