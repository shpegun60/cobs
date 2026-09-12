#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Mutation tests for the independent real-FreeRTOS raw-evidence verifier."""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import struct

import run
import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    assert __debug__, "python -O is not permitted"
    receipt = json.loads((args.directory / "session.json").read_text())
    original = [json.loads(line) for line in (args.directory / "results.jsonl").read_text().splitlines()]
    images = receipt["images"]
    verify.check_rows(original, images)
    checks = 0

    def reject(mutate):
        nonlocal checks
        rows = deepcopy(original)
        mutate(rows)
        try:
            verify.check_rows(rows, images)
        except (AssertionError, ValueError, IndexError):
            checks += 1
        else:
            raise AssertionError("mutated evidence accepted")

    def status(rows, name, index, value):
        row = next(r for r in rows if r["name"] == name)
        body = verify.decode(bytes.fromhex(row["rx"]), row["protocol"], row["policy"])
        words = list(struct.unpack("<24I", body[5:]))
        words[index] = value
        row["rx"] = run.encode(body[:5] + struct.pack("<24I", *words), row["protocol"], row["policy"]).hex()

    reject(lambda rows: rows.pop(3))
    reject(lambda rows: rows.append(deepcopy(rows[2])))
    reject(lambda rows: rows[2].update(image_sha256="0" * 64))
    reject(lambda rows: rows[2].update(name="vector-999"))
    reject(lambda rows: rows[2].update(body="ff"))
    reject(lambda rows: rows[2].update(rx=""))
    reject(lambda rows: rows[2].update(status="failed"))
    for index, value in ((0, 2), (3, 480000000), (5, 100500), (7, 1), (8, 19),
                         (9, 1), (10, 1), (11, 0), (12, 1), (13, 0), (15, 0),
                         (16, 1), (19, 2), (20, 0), (23, 2)):
        reject(lambda rows, i=index, v=value: status(rows, "hello", i, v))
    reject(lambda rows: status(rows, "after-idle", 17, 39))
    reject(lambda rows: status(rows, "after-idle", 18, 1))
    reject(lambda rows: status(rows, "after-idle", 14, 0))
    reject(lambda rows: status(rows, "busy-check", 6, 25))
    reject(lambda rows: next(r for r in rows if r["name"] == "after-idle").update(elapsed_seconds=0.001))
    for index, value in ((19, 3), (21, 3), (22, 2)):
        reject(lambda rows, i=index, v=value: status(rows, "partial-detach", i, v))
    reject(lambda rows: next(r for r in rows if r["name"] == "partial-detach").update(tx="0000"))
    assert verify.crc16(b"123456789") == bytes.fromhex("374b")
    for protocol in (0, 1, 2):
        for policy in (0, 1, 2):
            for body in (b"", b"\0", b"AB", bytes(range(250))):
                frame = run.encode(body, protocol, policy)
                assert verify.decode(frame, protocol, policy) == body
                checks += 1
    print(f"PASS {checks} verifier mutations/oracle checks; positive evidence accepted")


if __name__ == "__main__":
    main()
