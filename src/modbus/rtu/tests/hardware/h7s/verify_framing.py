#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Independently recheck a run_framing.py record and print its Markdown table.

Structural facts are enforced: every (mode, baud) has its smoke, framing and
vectors records, every record names the image that was flashed, the session
receipt binds the results file to those images and proves the firmware was
restored, and every source hash is a committed version at or after the
record's base commit (wire/tests/provenance.py). The frame-boundary outcomes
are printed, and the framed endpoint's claim — every single, split and glued
frame echoed exactly at every baud — is checked; the default endpoint's
losses are observations, reported as recorded.
"""
from __future__ import annotations

import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
def repository_root(start: Path) -> Path:
    """The git root: the directory holding COBS.pro, found by walking up from `start`."""
    for candidate in (start, *start.parents):
        if (candidate / "COBS.pro").is_file():
            return candidate
    raise RuntimeError(f"repository root (COBS.pro) not found above {start}")


REPO = repository_root(Path(__file__).resolve().parent)   # the git root
SRC = REPO / "src"
sys.path.insert(0, str(SRC / "wire/tests"))
from provenance import Provenance  # noqa: E402

SUITES = ("smoke", "framing", "vectors")
SHAPES = ("single", "split", "glued")
OPTIONAL_SHAPES = ("orphan",)  # added to the suite later; older records lack it


def verify(results: Path):
    rows = [json.loads(line) for line in results.read_text(encoding="utf-8").splitlines()]
    receipt = json.loads((results.parent / (results.name + ".session.json")).read_text(encoding="utf-8"))
    assert receipt["completed"] and receipt["restored_and_verified"] and receipt["backup_bytes"] == 65536
    assert receipt["results_sha256"] == hashlib.sha256(results.read_bytes()).hexdigest(), "results file differs from the receipt"
    images = {(i["mode"], i["baud"]): i for i in receipt["images"]}
    assert len(images) == len(receipt["modes"]) * len(receipt["bauds"])
    exits = {(s["mode"], s["baud"], s["suite"]): s["exit_code"] for s in receipt["suites"]}
    provenance = Provenance(REPO)
    seen = {}
    for row in rows:
        key = (int(row["framer"]), row["baud"], row["suite"])
        assert key not in seen, f"duplicate record {key}"
        assert key in exits and row["status"] in ("passed", "failed"), f"unrecognized suite outcome {key}"
        assert (exits[key] == 0) == (row["status"] == "passed"), f"record/exit status mismatch {key}"
        seen[key] = row
        image = row["image"]
        assert image is not None and bool(image["framer"]) == bool(row["framer"]) and image["baud"] == row["baud"]
        assert image["binary_sha256"] == images[(int(row["framer"]), row["baud"])]["binary_sha256"], \
            f"record {key} names an image the receipt did not flash"
        assert image["policy"] == receipt["policy"] and image["optimization"] == "-Os" and not image["lto"]
        provenance.check(image["source_base_commit"], image["source_sha256"])
        if row["suite"] == "framing":
            if row["status"] == "failed":
                # Burst framing can lose even a control response through VCP.
                # The failed run is evidence of that limitation, not a set of
                # successful (or zero-success) trials. Keep its cells unavailable.
                assert not row["framer"], f"framed framing suite failed at {row['baud']}"
                continue
            for shape in SHAPES + tuple(s for s in OPTIONAL_SHAPES if s in row["summary"]):
                trials = [t for t in row["trials"] if t["shape"] == shape]
                assert len(trials) == 12 and row["summary"][shape] == f"{sum(t['exact'] for t in trials)}/12"
    for mode in receipt["modes"]:
        for baud in receipt["bauds"]:
            for suite in SUITES:
                assert (mode, baud, suite) in exits, f"receipt lacks the {suite} run for framer={mode} at {baud}"
                if (mode, baud, suite) in seen:
                    continue
                # A run that failed before its first frame (the HELLO itself timed
                # out) leaves no record, only the receipt's exit code. Acceptable
                # only for the default endpoint, whose high-baud losses are the
                # observation this record exists to make.
                assert mode == 0 and exits[(mode, baud, suite)] != 0,                     f"missing {suite} record for framer={mode} at {baud}"
                seen[(mode, baud, suite)] = dict(status="failed", error="no HELLO response (recorded exit code "
                                                 f"{exits[(mode, baud, suite)]})", framer=bool(mode), baud=baud, suite=suite)
            if mode:
                assert seen[(mode, baud, "smoke")]["status"] == "passed", f"framed smoke failed at {baud}"
            # The default endpoint's smoke at high baud is an observation: the
            # bridge may split even the 21-byte smoke frame.
    framed_exact = all(t["exact"] for row in rows if row["suite"] == "framing" and row["framer"] for t in row["trials"])
    framed_vectors = all(seen[(1, baud, "vectors")]["status"] == "passed" for baud in receipt["bauds"] if 1 in receipt["modes"])
    provenance.report()
    return rows, receipt, seen, framed_exact, framed_vectors


def brief(error):
    """A table cell for a recorded failure: which vector, or why nothing came back."""
    match = re.match(r"vector (\d+) failed: data_size=(\d+)", error)
    if match:
        return f"at vector {match[1]} ({match[2]} data bytes), no echo"
    return "no HELLO response" if "HELLO" in error else error[:40]


def table(receipt, seen):
    summaries = [seen[(mode, baud, "framing")].get("summary", {})
                 for mode in receipt["modes"] for baud in receipt["bauds"]]
    orphan = any(summaries) and all("orphan" in summary for summary in summaries if summary)
    print("\n### RTU frame boundaries on the H7S ST-Link bridge: default burst framing versus the framing policy\n")
    print("| Baud | Endpoint | single-write echoes | split-write echoes | two frames in one write |"
          + (" orphan half then a whole frame |" if orphan else "") + " smoke | vectors suite |")
    print("|---:|---|---:|---:|---:|" + ("---:|" if orphan else "") + "---|---|")
    for baud in receipt["bauds"]:
        for mode in receipt["modes"]:
            framing = seen[(mode, baud, "framing")]
            vectors = seen[(mode, baud, "vectors")]
            smoke = seen[(mode, baud, "smoke")]
            name = "framing policy (length-prefixed)" if mode else "framing::None (burst candidate)"
            outcome = "passed" if vectors["status"] == "passed" else "FAILED " + brief(vectors.get("error", ""))
            smoke_outcome = "passed" if smoke["status"] == "passed" else "FAILED " + brief(smoke.get("error", ""))
            unavailable = "unavailable: " + brief(framing.get("error", "suite failed"))
            summary = framing.get("summary", {}) if framing["status"] == "passed" else {}
            print(f"| {baud} | {name} | {summary.get('single', unavailable)} | {summary.get('split', unavailable)} | "
                  f"{summary.get('glued', unavailable)} | "
                  + (f"{summary.get('orphan', unavailable)} | " if orphan else "")
                  + f"{smoke_outcome} | {outcome} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--check-doc", type=Path, help="every generated table row must appear in this document")
    args = parser.parse_args()
    rows, receipt, seen, framed_exact, framed_vectors = verify(args.results)
    print(f"PASS {len(rows)} records; {len(receipt['images'])} flashed images bound by the receipt; firmware restored and read back")
    print(f"{'PASS' if framed_exact else 'FAIL'} framed endpoint: every single/split/glued frame echoed exactly at every baud")
    print(f"{'PASS' if framed_vectors else 'FAIL'} framed endpoint: vectors suite passed at every baud")
    if args.check_doc:
        generated = io.StringIO()
        with redirect_stdout(generated):
            table(receipt, seen)
        lines = {line for line in generated.getvalue().splitlines() if line.startswith("|")}
        published = set(args.check_doc.read_text(encoding="utf-8").splitlines())
        assert lines <= published, f"missing/stale document rows: {lines - published}"
        print(f"PASS {len(lines)} table rows present in {args.check_doc}")
    table(receipt, seen)
    return 0 if framed_exact and framed_vectors else 1


if __name__ == "__main__":
    sys.exit(main())
