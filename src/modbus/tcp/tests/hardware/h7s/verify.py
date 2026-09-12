#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Independent MBAP/CRC byte oracle and complete live-record acceptance."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
CONFIGS = ("0-0", "1-0", "0-1", "0-2", "1-2", "0-3")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def width(policy):
    return (0, 2, 2, 4)[policy]


def frame(policy, transaction, unit, function, data):
    body = struct.pack(">HHHBB", transaction, 0, 2 + len(data) + width(policy), unit, function) + data
    if policy in (1, 2):
        crc = 0xFFFF
        for byte in body:
            crc ^= byte
            for _ in range(8):
                crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
        return body + struct.pack("<H", crc)
    if policy == 3:
        return body + struct.pack("<I", zlib.crc32(body))
    return body


def positive_cases(policy, schema=1):
    cases = []
    maximum = 1024 if schema == 2 else 1016 - width(policy)
    lengths = (0, 1, 2, 4, 31, 32, 63, 64, 127, 128, 249, 250, 251, 252, 253, 254, 255, 256, 257, 511, maximum)
    for length in lengths:
        for pattern in range(3):
            data = bytes((0 if pattern == 0 else i & 255 if pattern == 1 else (i * 73 + i // 7 + 19) & 255) for i in range(length))
            wire = frame(policy, 0x1234, 0xFE, 0x67, data)
            cases.append((f"length-{length}-pattern-{pattern}", [wire], wire))
    for function in range(256):
        wire = frame(policy, 0xFF00 + function, function, function, bytes((0, 255, 0x41, 0x42)))
        cases.append((f"function-{function}", [wire], wire))
    wire = frame(policy, 0x8001, 0, 0x83, bytes(range(32)))
    for split in (*range(1, 9), len(wire) - 1):
        cases.append((f"split-{split}", [wire[:split], wire[split:]], wire))
    train = b"".join(frame(policy, i, i + 1, 0x41, bytes(range(n))) for i, n in enumerate((0, 32, 80)))
    cases.append(("three-coalesced", [train], train))
    return cases


def negative_cases(policy, schema=1):
    valid = frame(policy, 7, 2, 3, b"\x00\x01")
    cases = [("protocol-id", b"\x00\x01\x00\x01\x00\x02"),
             ("length-zero", b"\x00\x01\x00\x00\x00\x00"),
             ("length-one", b"\x00\x01\x00\x00\x00\x01"),
             ("oversize", b"\x00\x01\x00\x00\xff\xff")]
    if schema == 2:
        cases.append(("data-limit-plus-one", struct.pack(">HHH", 1, 0, 1025 + 2 + width(policy))))
    if policy:
        corrupt = bytearray(valid)
        corrupt[-1] ^= 1
        cases.append(("crc-corrupt", bytes(corrupt)))
        # Plausible but wrong Length: must not publish a truncated packet.
        shorter = bytearray(valid)
        shorter[5] -= 1
        cases.append(("plausible-length-corrupt", bytes(shorter)))
    return [(name, bad + valid) for name, bad in cases]


def verify(path, local=False):
    record = json.loads(Path(path).read_text())
    schema = record["schema"]
    assert schema in (1, 2) and record["completed"]
    if schema == 2:
        assert record["limit_kind"] == "function-data bytes" and record["max_data_size"] == 1024
    assert record["transport"] == "UART byte transport; no TCP/IP stack"
    assert record["restored_and_verified"] and record["backup_bytes"] == 65536
    assert record["backup_sha256"] == record["readback_sha256"]
    assert len(record["images"]) == len(CONFIGS)
    for row, config in zip(record["images"], CONFIGS):
        assert row["config"] == config
        heap, policy = map(int, config.split("-"))
        hello = row["hello"].split(",")
        assert hello[:6] == ["TCP", str(schema), "600000000", str(heap), str(policy), "1024"]
        assert int(hello[6]) & 0x30000 == 0x30000  # D/I-cache enabled
        if schema == 2:
            assert len(hello) == 8 and int(hello[7]) == 1032 + width(policy)
        self_test = row["self_test"].split(",")
        assert self_test[0] == "SELF" and int(self_test[1]) >= 4000 and self_test[2] == "0"
        assert int(self_test[3]) >= (14 if heap else 2) and self_test[4] == "0"
        if schema == 2:
            assert len(self_test) == 7 and int(self_test[5]) == 53 and self_test[6] == "0"
        plan = positive_cases(policy, schema)
        assert len(row["positive"]) == len(plan)
        for actual, (name, chunks, expected) in zip(row["positive"], plan):
            assert actual["name"] == name and actual["chunks"] == [b.hex() for b in chunks]
            assert actual["received"] == expected.hex(), (config, name)
        negatives = negative_cases(policy, schema)
        assert len(row["negative"]) == len(negatives)
        for actual, (name, sent) in zip(row["negative"], negatives):
            assert actual["name"] == name and actual["sent"] == sent.hex() and actual["received"] == ""
            assert actual["baseline"] == frame(policy, 3, 1, 3, b"\x00\x01").hex()
            assert actual["boot_after"] == row["hello"]
        assert 0 < row["binary_bytes"] <= 65536
        assert row["artifacts"].keys() == {"elf", "bin", "map", "dis", "nm"}
        if local:
            root = Path(record["session"])
            for suffix, expected in row["artifacts"].items():
                assert digest(root / config / ("bench." + suffix)) == expected
        print(f"{config}: MCU self-tests + {len(plan)} exact UART exchanges + {len(negatives)} fail-closed trials")
    if local:
        for relative, expected in record["source_sha256"].items():
            assert digest(REPO / relative) == expected, f"source drift: {relative}"
        for relative, expected in record["log_sha256"].items():
            assert digest(Path(record["session"]) / relative) == expected
    print("PASS MBAP/MCU/UART evidence; not socket/Ethernet interoperability")
    return record


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("record", type=Path)
    parser.add_argument("--local", action="store_true")
    args = parser.parse_args()
    if not __debug__:
        parser.error("Python -O is forbidden")
    verify(args.record, args.local)
