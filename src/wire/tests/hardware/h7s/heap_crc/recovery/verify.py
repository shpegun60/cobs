#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Verify real Heap OOM refusal, strong growth guarantee and recovery on H7S."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
spec = importlib.util.spec_from_file_location("baseline_oom_verifier", HERE.parent / "oom/verify.py")
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)
PLAN = ("M", "P", "Z", "C", "R", "F", "c", "r", "f", "G", "g", "J", "N")
FATAL = {"N"} # confirm the correction does not replace global operator new
RX = {"c": 0, "r": 1, "f": 2}
GROW = {"G": 0, "g": 1, "J": 2}
digest = baseline.digest


def crc16(data):
    value = 65535
    for byte in data:
        for bit in range(8):
            feedback = (value ^ (byte >> bit)) & 1
            value >>= 1
            if feedback: value ^= 0xA001
    return value.to_bytes(2, "little")


def wire(protocol, size):
    assert size in (2, 34)
    body = b"AB" + (bytes.fromhex("11223344") + bytes(28) if size == 34 else b"")
    if protocol:
        data = (b"\x11\x03" if protocol == 1 else b"\x11\x41" + size.to_bytes(2, "big")) + body
        return data + crc16(data)
    raw = bytes([len(body) + 2]) + body + crc16(body)
    out, block, code = bytearray([0]), 0, 1
    for byte in raw:
        if byte:
            out.append(byte); code += 1
        else:
            out[block] = code; block, code = len(out), 1; out.append(0)
    out[block] = code
    return bytes(out) + b"\0" # these deliberately small vectors never fill an FF block


def check_case(row):
    key = row["command"]
    assert key in PLAN
    hello = row["hello"].split(",")
    assert hello[0] == "HELLO" and len(hello) == 7
    version, clock, address, capacity, errors, handler = map(int, hello[1:])
    assert (version, clock, capacity, errors, handler) == (2, 600000000, 131072, 0, 0)
    assert 0x24000000 <= address < address + capacity <= 0x24071C00
    if key == "N":
        original = dict(row, hello=row["hello"].replace("HELLO,2,", "HELLO,1,", 1))
        baseline.check_case(original)
        return
    terminal = "MALLOC_NULL" if key == "M" else "POOL_OK" if key == "P" else "REFUSED"
    expected_tags = (["WIRE"] if key in RX else []) + ["EXHAUSTED", "BEFORE", terminal] + ["WIRE"] * (4 if key in GROW else 3) + ["RECOVERED"]
    assert [line.split(",")[0] for line in row["lines"]] == expected_tags, "refusal/recovery stage plan"
    seen_wires, exhausted = [], None
    for line in row["lines"]:
        fields = line.split(",")
        assert fields[1] == key
        stage = fields[0]
        if stage == "WIRE":
            assert len(fields) == 5
            protocol, size = map(int, fields[2:4])
            assert protocol in (0, 1, 2) and bytes.fromhex(fields[4]) == wire(protocol, size), "wire bytes/preserved prefix"
            seen_wires.append((protocol, size))
            continue
        assert len(fields) == 8
        count, payload, refusals, committed, failures, detail = map(int, fields[2:])
        assert refusals == 4 and 0 < committed <= capacity and failures == 0, "MCU check/heap failure"
        if stage == "RECOVERED":
            assert count == payload == detail == 0
        else:
            assert 0 < count < 256 and 120000 < payload < capacity
            cut = count, payload, refusals, committed, failures
            if exhausted is None: exhausted = cut
            assert cut == exhausted, "failed operation changed ownership"
            expected_detail = 0
            if stage == "BEFORE" and key in GROW: expected_detail = 4 if key == "J" else 2
            if stage == "REFUSED": expected_detail = 5 if key in GROW else 3 if key in RX else 6 if key == "Z" else 1
            assert detail == expected_detail
    expected_wires = ([(RX[key], 2)] if key in RX else []) + ([(GROW[key], 34)] if key in GROW else []) + [(0, 2), (1, 2), (2, 2)]
    assert seen_wires == expected_wires
    assert row["after_ping"] == row["hello"], "firmware did not return to command loop"


def check_disassembly(text):
    baseline.check_disassembly(text)
    blocks = re.split(r"(?m)(?=^[0-9a-f]+ <)", text)
    functions = {re.match(r"^[0-9a-f]+ <(.+)>:", b)[1]: b for b in blocks if re.match(r"^[0-9a-f]+ <(.+)>:", b)}
    for name in ("heap_rx_probe", "heap_tx_probe", "heap_rx_release_probe", "heap_tx_release_probe"):
        pending, seen, found = [name], set(), False
        expected = "free" if "release" in name else "malloc"
        while pending:
            target = pending.pop()
            if target in seen: continue
            seen.add(target)
            assert target in functions, target
            block = functions[target]
            assert "operator new" not in block and "operator delete" not in block and "abort" not in block, "Heap depends on throwing C++ runtime"
            for called in re.findall(r"\b(?:bl|b\.w|b\.n)\s+[0-9a-f]+\s+<(.+)>", block):
                if called == expected: found = True
                elif called.startswith(target + "+"): continue
                else:
                    assert called.startswith("wire::Heap::"), f"unexpected Heap helper {called}"
                    pending.append(called)
        assert found, f"no {expected} path from {name}"


def verify(directory, local=False):
    r = json.loads((directory / "session.json").read_text())
    assert r["schema"] == 1 and r["completed"] and "error" not in r
    assert r["plan"] == list(PLAN) and [c["command"] for c in r["cases"]] == list(PLAN)
    assert r["backup_bytes"] == 65536 and r["restored_and_verified"] and r["backup_sha256"] == r["readback_sha256"]
    assert 0 < r["binary_bytes"] <= 65536
    for case in r["cases"]: check_case(case)
    p, ordinary = baseline.Provenance(REPO), {}
    for relative, sha in r["source_sha256"].items():
        if relative.startswith(("libs/delegate/", "libs/spsc/")):
            a, b, path = relative.split("/", 2); root = a + "/" + b
            revision = subprocess.check_output(["git", "rev-parse", r["source_base_commit"] + ":" + root], cwd=REPO, text=True).strip()
            data = subprocess.check_output(["git", "show", f"{revision}:{path}"], cwd=REPO / root).replace(b"\r\n", b"\n")
            assert sha in {hashlib.sha256(b).hexdigest() for b in (data, data.replace(b"\n", b"\r\n"))}
        else: ordinary[relative] = sha
    p.check(r["source_base_commit"], ordinary)
    excerpt = directory / "allocator_path.dis"
    assert digest(excerpt) == r["allocator_path_sha256"]
    check_disassembly(excerpt.read_text())
    if local:
        session = Path(r["session"])
        for suffix, sha in r["artifact_sha256"].items(): assert digest(session / ("bench." + suffix)) == sha
        full = (session / "bench.dis").read_text()
        check_disassembly(full)
        for block in re.split(r"(?m)(?=^[0-9a-f]+ <)", excerpt.read_text()): assert block.strip() in full
        assert digest(session / "before.bin") == digest(session / "after.bin") == r["backup_sha256"]
    p.report()
    print("PASS: 12 Heap/Pool refusal/recovery cases, 42 exact wire frames, strong growth and retained Packet checks; global-new negative control unchanged; flash restored")
    return r


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path); parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    verify(args.directory, args.local_images)


if __name__ == "__main__": main()
