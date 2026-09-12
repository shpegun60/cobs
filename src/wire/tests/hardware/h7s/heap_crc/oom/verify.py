#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Verify OOM path observations, not merely a missing serial response."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
sys.path.insert(0, str(REPO / "src/wire/tests"))
from provenance import Provenance  # noqa: E402

PLAN = ("M", "P", "N", "C", "R", "c", "r", "G", "g")
FATAL = set(PLAN) - {"M", "P"}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_case(row):
    command = row["command"]
    assert command in PLAN
    hello = row["hello"].split(",")
    assert hello[0] == "HELLO" and len(hello) == 7
    version, core, begin, size, failures, handler = map(int, hello[1:])
    assert (version, core, size, failures, handler) == (1, 600000000, 131072, 0, 0)
    assert 0x24000000 <= begin < begin + size <= 0x24071C00
    parsed = []
    for line in row["lines"]:
        parts = line.split(",")
        assert len(parts) == 8 and parts[1] == command, "wrong stage/command shape"
        values = tuple(map(int, parts[2:]))
        count, live_bytes, refusals, committed, errors, detail = values
        assert errors == 0 and refusals == 4 and 0 < committed <= size
        if parts[0] == "RECOVERED":
            assert count == live_bytes == 0 and detail == 0
        else:
            assert 0 < count < 256 and 120000 < live_bytes < size
        parsed.append((parts[0], values))
    expected = ["EXHAUSTED", "BEFORE"] + (["ABORT", "EXIT"] if command in FATAL
        else ["MALLOC_NULL" if command == "M" else "POOL_OK", "RECOVERED"])
    assert [p[0] for p in parsed] == expected, "missing/extra/unexpected OOM path"
    initial = parsed[0][1][:5]
    assert all(values[:5] == initial for tag, values in parsed if tag != "RECOVERED"), "heap changed during observed failure"
    for tag, values in parsed:
        detail = values[5]
        if tag == "BEFORE" and command in ("G", "g"):
            assert 1 <= detail < 33, "growth was not required"
        else:
            assert detail == (1 if tag == "EXIT" else 0), "unexpected exit/detail"
    if command in FATAL:
        assert row["after_ping"] == "", "execution returned to command loop after _exit"
    else:
        assert row["after_ping"] == row["hello"], "control case failed to remain responsive"


def check_disassembly(text):
    blocks = re.split(r"(?m)(?=^[0-9a-f]+ <)", text)
    def function(name):
        found = [block for block in blocks if re.match(r"^[0-9a-f]+ <" + re.escape(name) + r">:", block)]
        assert len(found) == 1, f"missing/duplicate function {name}"
        return found[0]
    nothrow = function("operator new(unsigned int, std::nothrow_t const&)")
    assert re.search(r"\bb(?:\.w|\.n)?\s+\w+ <operator new\(unsigned int\)>", nothrow), "different nano allocation implementation"
    regular = function("operator new(unsigned int)")
    for target in ("malloc", "std::get_new_handler()", "__wrap_abort"):
        assert f"<{target}>" in regular
    assert "<__real_abort>" not in text, "linker did not resolve real abort"
    assert "<__real__exit>" not in text, "linker did not resolve real exit"
    assert "<abort>" in function("__wrap_abort")
    assert "<__wrap__exit>" in function("abort")
    assert "<_exit>" in function("__wrap__exit")
    assert "<_kill>" in function("_exit") or "<__errno>" in function("_exit")
    # Original generated _exit contains a terminal self-branch.
    assert any(m[1].lstrip("0") == m[2].lstrip("0") for m in re.findall(
        r"(?m)^\s*(([0-9a-f]+)):\s+[0-9a-f ]+\s+b(?:\.n|\.w)?\s+([0-9a-f]+)\b", function("_exit"))), "missing original _exit loop"


def verify(directory, local=False):
    record = json.loads((directory / "session.json").read_text())
    assert record["schema"] == 1 and record["completed"] and "error" not in record
    assert record["plan"] == list(PLAN) and [r["command"] for r in record["cases"]] == list(PLAN)
    assert record["backup_bytes"] == 65536 and record["restored_and_verified"]
    assert record["backup_sha256"] == record["readback_sha256"]
    assert 0 < record["binary_bytes"] <= 65536
    for row in record["cases"]: check_case(row)
    p = Provenance(REPO)
    ordinary = {}
    for relative, sha in record["source_sha256"].items():
        if relative.startswith(("libs/delegate/", "libs/spsc/")):
            a, b, path = relative.split("/", 2)
            root = a + "/" + b
            revision = subprocess.check_output(["git", "rev-parse", record["source_base_commit"] + ":" + root], cwd=REPO, text=True).strip()
            blob = subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=REPO / root)
            normalized = blob.replace(b"\r\n", b"\n")
            assert sha in {hashlib.sha256(x).hexdigest() for x in (blob, normalized, normalized.replace(b"\n", b"\r\n"))}
        else:
            ordinary[relative] = sha
    p.check(record["source_base_commit"], ordinary)
    excerpt = directory / "allocator_path.dis"
    assert digest(excerpt) == record["allocator_path_sha256"]
    check_disassembly(excerpt.read_text())
    if local:
        session = Path(record["session"])
        for suffix, sha in record["artifact_sha256"].items():
            assert digest(session / ("bench." + suffix)) == sha
        text = (session / "bench.dis").read_text()
        check_disassembly(text)
        for block in re.split(r"(?m)(?=^[0-9a-f]+ <)", excerpt.read_text()):
            assert block.strip() in text, "excerpt differs from measured ELF disassembly"
        assert digest(session / "before.bin") == digest(session / "after.bin") == record["backup_sha256"]
    p.report()
    print("CONFIRMED: 7 allocation paths enter real abort/_exit; malloc returns null; Pool and post-free Heap controls recover; original flash restored")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    verify(args.directory, args.local_images)


if __name__ == "__main__": main()
