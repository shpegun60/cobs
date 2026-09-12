#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Independently decode the raw real-FreeRTOS exchanges and check the exact plan.

Does not import the runner or trust its pass flags, telemetry summaries, or
an aggregate count in place of individual cases.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
sys.path.insert(0, str(REPO / "src/wire/tests"))
from provenance import Provenance  # noqa: E402

MAGIC = bytes.fromhex("b6505254")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def crc16(data):
    register = 65535
    for value in data:
        for bit in range(8):
            feedback = (register ^ (value >> bit)) & 1
            register >>= 1
            if feedback:
                register ^= 0xA001
    return register.to_bytes(2, "little")


def decode(frame, protocol, policy, check_crc=True):
    if protocol == 0:
        assert frame and frame[-1] == 0 and 0 not in frame[:-1], "invalid COBS delimiter"
        raw = bytearray()
        pos, end = 0, len(frame) - 1
        while pos < end:
            code = frame[pos]
            pos += 1
            assert pos + code - 1 <= end, "truncated COBS block"
            raw.extend(frame[pos:pos + code - 1])
            pos += code - 1
            if code < 255 and pos < end:
                raw.append(0)
        assert raw and raw[0] == len(raw) - 1, "COBS length mismatch"
        protected = bytes(raw[1:])
    else:
        assert frame[:2] == b"\x11\x41" and len(frame) >= 4, "RTU envelope"
        protected = frame
    data = protected[:-2] if policy else protected
    if policy and check_crc:
        assert protected[-2:] == crc16(data), "CRC mismatch"
    if protocol:
        assert int.from_bytes(data[2:4], "big") == len(data) - 4, "RTU prefix mismatch"
        data = data[4:]
    return data


def expected_plan(protocol, policy):
    maximum = (255 if protocol == 0 else 252) - (2 if policy else 0)
    entries = [("hello", "status", MAGIC + b"H")]
    entries += [(f"vector-{n}", "echo", bytes((i * 37 + n) & 255 for i in range(n)))
                for n in (0, 1, 4, 31, 32, 63, 127, maximum)]
    entries += [("busy", "status", MAGIC + b"B"), ("busy-check", "status", MAGIC + b"S")]
    if policy:
        entries += [("bad-crc", "bad-crc", b"intentionally-corrupt"), ("crc-recovery", "status", MAGIC + b"C")]
    if protocol != 1:
        entries += [("detach", "status", MAGIC + b"D"), ("partial-detach", "partial", bytes(range(100)))]
        if protocol == 0:
            entries.append(("resync", "resync", b""))
        entries.append(("detach-recovery", "status", MAGIC + b"R"))
    if protocol == 2:
        entries += [("orphan", "orphan", bytes(range(100))), ("deadline-recovery", "status", MAGIC + b"T")]
    entries += [(f"stress-{n}", "echo", bytes((n + i * 13) & 255 for i in range(80 + n))) for n in range(32)]
    entries += [("before-idle", "status", MAGIC + b"W"), ("after-idle", "status", MAGIC + b"I")]
    return entries


def check_rows(rows, images):
    keys = [(i["protocol"], i["policy"], i["baud"]) for i in images]
    assert len(keys) == len(set(keys)), "duplicate image"
    assert Counter((r["protocol"], r["policy"], r["baud"]) for r in rows) == Counter(
        {key: len(expected_plan(*key[:2])) for key in keys}), "missing/extra trials"
    for image in images:
        p, c, baud = image["protocol"], image["policy"], image["baud"]
        selected = [r for r in rows if (r["protocol"], r["policy"], r["baud"]) == (p, c, baud)]
        snapshots = {}
        for row, (name, kind, body) in zip(selected, expected_plan(p, c), strict=True):
            assert (row["name"], row["kind"], row["body"]) == (name, kind, body.hex()), "changed trial plan"
            assert row["status"] == "passed" and row["image_sha256"] == image["elf_sha256"]
            tx, rx = bytes.fromhex(row["tx"]), bytes.fromhex(row["rx"])
            if kind in ("partial", "orphan"):
                assert 45 <= len(tx) <= 55, "partial input not sent"
                if p == 0:
                    prefix = bytearray()
                    pos = 0
                    while pos < len(tx):
                        code = tx[pos]
                        assert code != 0
                        pos += 1
                        prefix.extend(tx[pos:min(len(tx), pos + code - 1)])
                        pos += code - 1
                        if code < 255 and pos < len(tx):
                            prefix.append(0)
                    assert prefix[0] == 100 + (2 if c else 0) and prefix[1:] == body[:len(prefix) - 1]
                else:
                    assert tx[:4] == bytes.fromhex("11410064") and tx[4:] == body[:len(tx) - 4]
                if kind == "orphan":
                    assert rx == b"" and row["elapsed_seconds"] >= 0.02
                    continue
            elif kind == "resync":
                assert tx == b"\0" and not rx
                continue
            elif kind == "bad-crc":
                assert decode(tx, p, c, False) == body[:-1] + bytes([body[-1] ^ 1])
                try:
                    decode(tx, p, c)
                except AssertionError:
                    pass
                else:
                    raise AssertionError("negative trial actually had a valid CRC")
                assert not rx and row["elapsed_seconds"] >= 0.4
                continue
            else:
                assert decode(tx, p, c) == body, "wrong request data"
            answer = decode(rx, p, c)
            if kind == "echo":
                assert answer == body, "echo data changed"
                continue
            assert len(answer) == 101 and answer[:4] == MAGIC
            assert answer[4] == (ord("d") if kind == "partial" else body[4])
            w = struct.unpack("<24I", answer[5:])
            version = image.get("firmware_version", 1)
            assert version in (1, 2), "unknown firmware contract"
            assert w[:6] == (version, p, c, 600000000, baud, 100602), "wrong firmware/platform/kernel"
            expected_checks = 114 if version == 2 else 20
            assert w[8] == expected_checks and w[7] == w[9] == w[10] == w[12] == w[16] == 0, "device failure"
            # Uart::init() itself calls receiveRestart(): exactly one startup
            # start, with no additional error-recovery restart during traffic.
            assert w[23] == 1, "unexpected UART restart after init"
            assert w[6] >= 25 and w[11] > 0 and w[13] > 0 and w[15] > 0 and w[20] > 0
            assert w[19] == (4 if kind == "partial" else 3), "RX pool leak"
            if kind == "partial":
                assert w[21:23] == (2, 3), "detach did not release just the partial packet"
            snapshots[name] = w
        first, last = snapshots["hello"], snapshots["after-idle"]
        assert last[17] == 40 and last[6] >= 70 and last[11] > first[11] and last[13] > first[13]
        assert last[18] == sum(kind in ("echo", "status") for _, kind, _ in expected_plan(p, c))
        assert snapshots["busy-check"][6] >= snapshots["busy"][6] + 3, "busy lifetime checks missing"
        assert last[14] >= snapshots["before-idle"][14] + 2 and last[15] > snapshots["before-idle"][15]
        assert selected[-1]["elapsed_seconds"] >= 0.15


def verify(directory, local_images=False):
    receipt = json.loads((directory / "session.json").read_text())
    results = directory / "results.jsonl"
    rows = [json.loads(line) for line in results.read_text().splitlines()]
    assert receipt["schema"] in (1, 2) and receipt["completed"] and receipt["restored_and_verified"]
    assert all(i.get("firmware_version", 1) == receipt["schema"] for i in receipt["images"])
    assert receipt["backup_bytes"] == 65536 and receipt["readback_sha256"] == receipt["backup_sha256"]
    assert digest(results) == receipt["results_sha256"]
    assert receipt["kernel_version"] == "V10.6.2" and len(receipt["kernel_sha256"]) > 10
    plan = [tuple(value) for value in receipt["plan"]]
    bauds = sorted({b for _, _, b in plan})
    assert bauds and all(b in (115200, 1000000) for b in bauds)
    expected = {(p, c, b) for p, c in ((0, 0), (0, 1), (0, 2), (1, 1), (2, 0), (2, 1), (2, 2)) for b in bauds}
    assert len(plan) == len(expected) and set(plan) == expected, "incomplete matrix"
    assert [(i["protocol"], i["policy"], i["baud"]) for i in receipt["images"]] == plan
    check_rows(rows, receipt["images"])
    provenance = Provenance(REPO)
    checked_deps = set()
    session = Path(receipt["session"])
    for image in receipt["images"]:
        ordinary = {}
        for relative, value in image["source_sha256"].items():
            if relative.startswith(("libs/delegate/", "libs/spsc/")):
                if (relative, value) in checked_deps:
                    continue
                parts = relative.split("/", 2)
                root, path = "/".join(parts[:2]), parts[2]
                entry = subprocess.check_output(["git", "ls-tree", receipt["source_base_commit"], root], cwd=REPO, text=True).split()
                assert entry[0] == "160000"
                blob = subprocess.check_output(["git", "show", f"{entry[2]}:{path}"], cwd=REPO / root)
                candidates = (blob, blob.replace(b"\n", b"\r\n")) if b"\r" not in blob else (blob,)
                assert any(hashlib.sha256(b).hexdigest() == value for b in candidates), relative
                checked_deps.add((relative, value))
            else:
                ordinary[relative] = value
        provenance.check(receipt["source_base_commit"], ordinary)
        assert 0 < image["binary_bytes"] <= 65536 and 1 <= image["flash"]["attempts"] <= 3
        if local_images:
            tag = image["tag"]
            for suffix, key in (("elf", "elf_sha256"), ("bin", "binary_sha256")):
                assert digest(session / (tag + "." + suffix)) == image[key]
            assert digest(session / ("build-" + tag + ".log")) == image["build_log_sha256"]
            log = session / ("flash-" + tag + "-" + str(image["flash"]["attempts"]) + ".log")
            assert digest(log) == image["flash"]["log_sha256"] and "Download verified successfully" in log.read_text()
    provenance.report()
    if local_images:
        assert digest(session / "before.bin") == digest(session / "after.bin") == receipt["backup_sha256"]
        for relative, value in receipt["kernel_sha256"].items():
            assert digest(Path(receipt["kernel_root"]) / relative) == value
        log = session / ("restore-" + str(receipt["restore_flash"]["attempts"]) + ".log")
        assert digest(log) == receipt["restore_flash"]["log_sha256"] and "Download verified successfully" in log.read_text()
    else:
        print("CAVEAT local ELF/flash/backup/kernel bytes not re-read; use --local-images on the measuring machine")
    local_checks = sum(114 if i.get("firmware_version", 1) == 2 else 20 for i in receipt["images"])
    print(f"PASS {len(receipt['images'])} real FreeRTOS images; {len(rows)} exchanges; "
          f"{40 * len(receipt['images'])} exact echoes; {local_checks} MCU-local contract checks; restored")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__:
        parser.error("python -O is not permitted")
    verify(args.directory, args.local_images)
