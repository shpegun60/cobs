#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Recheck a completed live-audit receipt, exact trial plan and device verdicts."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
sys.path.insert(0, str(REPO / "src/wire/tests"))
from provenance import Provenance  # noqa: E402


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without python -O; verification assertions must be enabled")
    receipt = json.loads((args.directory / "session.json").read_text())
    results = args.directory / "results.jsonl"
    rows = [json.loads(line) for line in results.read_text().splitlines()]
    assert receipt["completed"] and receipt["restored_and_verified"]
    assert receipt["backup_bytes"] == 65536 and receipt["backup_sha256"] == receipt["readback_sha256"]
    assert receipt["results_sha256"] == digest(results)
    assert [image["optimization"] for image in receipt["images"]] == receipt["optimizations"]
    expected = Counter()
    for opt in receipt["optimizations"]:
        for cmd in "HL":
            expected[(opt, 0, cmd)] += 1
        for repeat in range(1, receipt["repetitions"] + 1):
            for cmd in "QHDHZHPHAHEHFH":
                expected[(opt, repeat, cmd)] += 1
    assert Counter((r["optimization"], r["repetition"], r["command"]) for r in rows) == expected
    images = {i["optimization"]: i for i in receipt["images"]}
    checks = {"H": 6, "L": 265, "Q": 3, "D": 6, "Z": 5, "P": 7, "A": 9, "E": 10, "F": 8}
    for row in rows:
        cmd = row["command"]
        assert row["status"] == "passed" and row["failed"] == row["first_failed"] == 0
        assert row["checks"] == checks[cmd] and row["image_sha256"] == images[row["optimization"]]["elf_sha256"]
        terminal = row["lines"][-1].split()
        assert terminal[:3] == ["AUDIT", "T", cmd]
        assert list(map(int, terminal[3:])) == [0, row["checks"], 0, 0, *row["observations"]]
        ready = [int(line.split()[3]) for line in row["lines"][:-1]]
        assert ready == {"Z": [1], "P": [1, 2], "A": [1, 2, 3, 4], "E": [1, 4], "F": [1]}.get(cmd, [])
        write_sizes = [w["bytes"] for w in row["writes"]]
        assert write_sizes == {"Z": [1, 256], "P": [1, 256, 1], "A": [1, 256, 1, 1, 48],
                               "E": [1, 256, 50], "F": [1, 306]}.get(cmd, [1])
        a, b, c, d = row["observations"]
        if cmd == "H":
            assert (a, b, c) == (600000000, 9600, 256) and d & (3 << 16) == 3 << 16
        elif cmd in "DZ":
            assert 350 <= a <= 1800 and b == c == 1 and d == (cmd == "Z")
        elif cmd == "P":
            assert a == b == 1 and 640 <= c <= 670 and d == 256
        elif cmd == "A":
            assert a == b == 2 and d == 306
        elif cmd in "EF":
            assert d == 306
    provenance = Provenance(REPO)
    dependencies = {}
    checkout_matches = set()
    for relative in ("libs/delegate", "libs/spsc"):
        entry = subprocess.check_output(["git", "ls-tree", receipt["source_base_commit"], relative], cwd=REPO, text=True).split()
        assert entry[0] == "160000", f"expected a pinned submodule: {relative}"
        dependencies[relative + "/"] = (entry[2], Provenance(REPO / relative))
    for image in receipt["images"]:
        assert image["binary_bytes"] <= 65536 and 1 <= image["flash"]["attempts"] <= 3
        ordinary = dict(image["source_sha256"])
        for prefix, (revision, dependency) in dependencies.items():
            selected = {key[len(prefix):]: ordinary.pop(key) for key in list(ordinary) if key.startswith(prefix)}
            # Nested repos use core.autocrlf rather than this repo's eol=lf.
            # Reconstruct the pinned blob's exact LF/CRLF checkout bytes, so a
            # Linux verifier can still verify a Windows measurement. The
            # measured hash is never normalized or changed. No other whitespace
            # or source difference is allowed by this special case.
            unresolved = {}
            for relative, expected_hash in selected.items():
                key = (prefix, relative, expected_hash)
                if key in checkout_matches:
                    continue
                blob = subprocess.run(["git", "show", f"{revision}:{relative}"],
                                      cwd=REPO / prefix, capture_output=True)
                variants = [blob.stdout]
                if b"\r" not in blob.stdout:
                    variants.append(blob.stdout.replace(b"\n", b"\r\n"))
                if blob.returncode == 0 and any(hashlib.sha256(v).hexdigest() == expected_hash for v in variants):
                    checkout_matches.add(key)
                else:
                    unresolved[relative] = expected_hash
            if unresolved:
                dependency.check(revision, unresolved)
        provenance.check(receipt["source_base_commit"], ordinary)
        if args.local_images:
            session = Path(receipt["session"])
            assert digest(session / ("audit" + image["optimization"] + ".elf")) == image["elf_sha256"]
            assert digest(session / ("audit" + image["optimization"] + ".bin")) == image["binary_sha256"]
            assert digest(session / ("build" + image["optimization"] + ".log")) == image["build_log_sha256"]
            log = session / ("flash" + image["optimization"] + "-" + str(image["flash"]["attempts"]) + ".log")
            assert digest(log) == image["flash"]["log_sha256"] and "Download verified successfully" in log.read_text()
            assert digest(session / "before.bin") == digest(session / "after.bin") == receipt["backup_sha256"]
    provenance.report()
    for prefix, (revision, dependency) in dependencies.items():
        print(f"dependency {prefix} pinned at {revision}: "
              f"{sum(key[0] == prefix for key in checkout_matches)} identities match its checkout bytes")
        if dependency.checked:
            dependency.report()
    print(f"PASS {len(rows)} live trials, {sum(r['checks'] for r in rows)} device assertions, "
          f"{len(images)} flashed images; original 64 KiB restored and read back")
    if not args.local_images:
        print("CAVEAT retained ELF/flash-log/backup bytes not re-read; use --local-images on the measuring machine")


if __name__ == "__main__":
    main()
