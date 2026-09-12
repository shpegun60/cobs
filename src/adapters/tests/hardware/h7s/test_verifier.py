#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Mutation checks for verify.py. Uses a completed record; never opens hardware."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without python -O")
    base_receipt = json.loads((args.directory / "session.json").read_text())
    base_rows = [json.loads(line) for line in (args.directory / "results.jsonl").read_text().splitlines()]
    # Each mutation updates the result-file digest so the semantic validator,
    # rather than merely the outer hash, must reject the damaged evidence.
    def mutate(name, receipt, rows):
        if name == "not-restored":
            receipt["restored_and_verified"] = False
        elif name == "missing-trial":
            rows.pop()
        elif name == "failed-assertion":
            rows[0]["failed"] = 1
        elif name == "missing-assertion":
            rows[0]["checks"] -= 1
        elif name == "wrong-image":
            rows[0]["image_sha256"] = "0" * 64
        elif name == "wrong-dma-count":
            rows[0]["observations"][2] = 0
            words = rows[0]["lines"][-1].split()
            words[-2] = "0"
            rows[0]["lines"][-1] = " ".join(words)
        elif name == "missing-ready":
            next(row for row in rows if row["command"] == "P")["lines"].pop(0)
        elif name == "short-physical-input":
            next(row for row in rows if row["command"] == "Z")["writes"][-1]["bytes"] = 255
        elif name == "readback-mismatch":
            receipt["readback_sha256"] = "0" * 64
        else:
            raise AssertionError(name)

    cases = ("not-restored", "missing-trial", "failed-assertion", "missing-assertion", "wrong-image",
             "wrong-dma-count", "missing-ready", "short-physical-input", "readback-mismatch")
    with tempfile.TemporaryDirectory(prefix="h7s-audit-verifier-") as temporary:
        folder = Path(temporary)
        for name in cases:
            receipt, rows = copy.deepcopy(base_receipt), copy.deepcopy(base_rows)
            mutate(name, receipt, rows)
            results = folder / "results.jsonl"
            results.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            receipt["results_sha256"] = hashlib.sha256(results.read_bytes()).hexdigest()
            (folder / "session.json").write_text(json.dumps(receipt), encoding="utf-8")
            run = subprocess.run([sys.executable, "-B", str(HERE / "verify.py"), str(folder)],
                                 capture_output=True, text=True, timeout=30)
            assert run.returncode != 0 and "AssertionError" in run.stderr, f"mutation accepted: {name}"
            print(f"PASS verifier rejects {name}")
    print(f"PASS {len(cases)} evidence mutations; no hardware accessed")


if __name__ == "__main__":
    main()
