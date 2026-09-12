#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Fresh-backup/build/flash/live-fault/read-back-restore H7S audit regression.

No other program may own the board or COM port during this run. Every result
includes its flashed image and byte-exact source manifest. A failing trial
is retained, makes the command fail, and still enters firmware restoration.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import serial

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
PROJECT = REPO / "stm32_cube_test/h7s_cobs_test"
PROGRAMMER = "C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
CHECKS = {"H": 6, "L": 265, "Q": 3, "D": 6, "Z": 5, "P": 7, "A": 9, "E": 10, "F": 8,
          "I": 6, "J": 11, "K": 11}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def frame():
    # Independent Python CRC reference; no imports from library runners.
    data = bytes((0x11, 0x41, 1, 44)) + bytes((i * 37 + 0x5A) & 255 for i in range(300))
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return data + crc.to_bytes(2, "little")


def sources():
    files = {HERE / "audit_bench.cpp", HERE / "build.sh", Path(__file__)}
    for folder in (REPO / "src/uart", REPO / "src/modbus", REPO / "src/wire", REPO / "src/crc",
                   REPO / "src/adapters/rtu", REPO / "libs/delegate", REPO / "libs/spsc"):
        for suffix in ("*.h", "*.hpp"):
            files.update(p for p in folder.rglob(suffix) if "tests" not in p.relative_to(folder).parts)
    files.add(REPO / "src/uart/tests/bench/uart_bench.h")
    for folder in (PROJECT / "Boot/Core", PROJECT / "Boot/Core/Startup", PROJECT / "Drivers"):
        for suffix in ("*.h", "*.c", "*.s"):
            files.update(folder.rglob(suffix))
    files.add(PROJECT / "Boot/STM32H7S3L8HX_FLASH.ld")
    return {str(p.relative_to(REPO)).replace("\\", "/"): digest(p) for p in sorted(files)}


def trial(port, command, output, optimization, image_hash, repetition):
    row = dict(command=command, optimization=optimization, image_sha256=image_hash,
               repetition=repetition, status="running", lines=[], writes=[])
    begin = time.monotonic()

    def write(data):
        written = port.write(data)
        assert written == len(data), "short serial write"
        port.flush()
        row["writes"].append(dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))

    try:
        write(command.encode("ascii"))
        previous_stage = 0
        while time.monotonic() - begin < 7.0:
            line = port.readline()
            if not line:
                continue
            if line == b"\n" and command in "JK":
                row["fault_newlines"] = row.get("fault_newlines", 0) + 1
                assert row["fault_newlines"] <= 64, "unexpected fault preamble size"
                continue
            text = line.decode("ascii").strip()
            row["lines"].append(text)
            words = text.split()
            assert len(words) == 11 and words[0] == "AUDIT" and words[2] == command, f"unexpected report: {text!r}"
            stage, count, failed, first, a, b, c, d = map(int, words[3:])
            if words[1] == "R":
                assert stage > previous_stage, "duplicate or unordered READY"
                previous_stage = stage
                if stage == 1 and command == "I":
                    write(b"DMA-live")
                elif stage == 1 and command == "Z":
                    write(bytes([0x5A]) * 256)
                elif stage == 1 and command in "PAE":
                    write(frame()[:256])
                elif stage == 1 and command == "F":
                    write(frame())
                elif stage == 2 and command in "PA":
                    write(frame()[256:257])
                elif stage == 3 and command == "A":
                    write(frame()[257:258])
                elif stage == 4 and command in "AE":
                    write(frame()[258 if command == "A" else 256:])
                else:
                    raise AssertionError(f"unexpected READY: {text}")
            elif words[1] == "T":
                row.update(checks=count, failed=failed, first_failed=first, observations=[a, b, c, d])
                assert failed == 0 and first == 0, f"device assertion: {text}"
                assert count == CHECKS[command], f"missing checks: {text}"
                if command == "H":
                    assert a == 600000000 and b == 9600 and c == 256 and d & (3 << 16) == 3 << 16
                if command in "IJK":
                    assert (a, b, c, d) == (1, 1, 0, 0 if command == "I" else 1)
                if command in "DZ":
                    assert 350 <= a <= 1800 and b == c == 1
                    assert d == (1 if command == "Z" else 0)
                if command == "P":
                    assert a == b == 1 and 640 <= c <= 670 and d == 256
                if command == "A":
                    assert a == b == 2 and d == 306
                if command in "EF":
                    assert d == 306
                row["status"] = "passed"
                return row
            else:
                raise AssertionError(f"unexpected report kind: {text}")
        raise TimeoutError(f"no terminal report for {command}")
    except BaseException as exc:
        row.update(status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        row["elapsed_seconds"] = time.monotonic() - begin
        with output.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(row, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new results directory")
    parser.add_argument("--optimizations", default="s,2,3")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--programmer", default=PROGRAMMER)
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without python -O; acceptance assertions must be enabled")
    if not re.fullmatch(r"COM[0-9]+", args.port) or not re.fullmatch(r"[A-Za-z0-9]+", args.serial):
        parser.error("expected COM port and alphanumeric ST-Link serial")
    opts = ["-O" + opt for opt in args.optimizations.split(",")]
    if not opts or any(opt not in ("-Os", "-O2", "-O3") for opt in opts) or args.repetitions < 1:
        parser.error("invalid optimization or repetitions")
    output = args.output.resolve()
    if output.exists():
        parser.error("refusing to overwrite an existing results directory")
    output.mkdir(parents=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = PROJECT / "out" / ("paranoid-" + stamp)
    session.mkdir()
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]
    receipt = dict(schema=2, started=stamp, port=args.port, serial=args.serial, optimizations=opts,
                   repetitions=args.repetitions, session=str(session), images=[], completed=False,
                   restored_and_verified=False, source_base_commit=subprocess.check_output(
                       ["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip())

    def save():
        (output / "session.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def execute(tag, argv, env=None, timeout=120):
        log = session / (tag + ".log")
        with log.open("w", encoding="utf-8") as stream:
            proc = subprocess.run(argv, cwd=REPO, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        return proc.returncode, log

    def flash(tag, path, binary=False):
        for attempt in range(1, 4):
            code, log = execute(f"{tag}-{attempt}", [args.programmer, *connection, "-w", str(path),
                                *(["0x08000000"] if binary else []), "-v", "-rst"], timeout=60)
            if code == 0 and "Download verified successfully" in log.read_text(encoding="utf-8", errors="replace"):
                return dict(attempts=attempt, log_sha256=digest(log))
        raise RuntimeError(f"flash/verify failed: {tag}, see {session}")

    backup = session / "before.bin"
    code, _ = execute("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    if code or not backup.is_file() or backup.stat().st_size != 65536:
        raise RuntimeError("valid 64 KiB backup required before flashing")
    receipt.update(backup_sha256=digest(backup), backup_bytes=65536)
    save()
    try:
        for opt in opts:
            print(f"BUILD/FLASH {opt}", flush=True)
            environment = os.environ.copy()
            environment["AUDIT_OPT"] = opt
            before = sources()
            code, log = execute("build" + opt, ["C:/Program Files/Git/bin/bash.exe", str(HERE / "build.sh")], environment)
            if code:
                raise RuntimeError(f"build failed: {log}")
            assert sources() == before, "source files changed during the build"
            build = PROJECT / "out" / ("paranoid-hardware" + opt)
            for suffix in ("elf", "bin", "map", "dis"):
                shutil.copy2(build / ("audit_bench." + suffix), session / ("audit" + opt + "." + suffix))
            for name in ("audit_bench.cpp", "build.sh", "run.py"):
                shutil.copy2(HERE / name, session / (opt + "-" + name))
            elf = session / ("audit" + opt + ".elf")
            binary = session / ("audit" + opt + ".bin")
            assert binary.stat().st_size <= 65536
            image = dict(optimization=opt, elf_sha256=digest(elf), binary_sha256=digest(binary),
                         binary_bytes=binary.stat().st_size, source_sha256=before, build_log_sha256=digest(log))
            image["flash"] = flash("flash" + opt, elf)
            receipt["images"].append(image)
            save()
            time.sleep(0.6)
            with serial.Serial(args.port, 9600, timeout=0.2, write_timeout=2) as port:
                port.reset_input_buffer()
                for cmd in ("H", "L"):
                    row = trial(port, cmd, output / "results.jsonl", opt, image["elf_sha256"], 0)
                    print(f"PASS {opt} {cmd}: {row['checks']} checks", flush=True)
                for repetition in range(1, args.repetitions + 1):
                    for cmd in "QHIHJHKHDHZHPHAHEHFH":
                        trial(port, cmd, output / "results.jsonl", opt, image["elf_sha256"], repetition)
                    print(f"PASS {opt} fault/control round {repetition}", flush=True)
        receipt["completed"] = True
    except BaseException as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        try:
            receipt["restore_flash"] = flash("restore", backup, binary=True)
            restored = session / "after.bin"
            code, _ = execute("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(restored), "-rst"])
            receipt["readback_sha256"] = digest(restored) if restored.is_file() else None
            receipt["restored_and_verified"] = code == 0 and receipt["readback_sha256"] == receipt["backup_sha256"]
        finally:
            results = output / "results.jsonl"
            if results.exists():
                receipt["results_sha256"] = digest(results)
            save()
            print(f"RESTORED_AND_VERIFIED={receipt['restored_and_verified']} session={session}", flush=True)
    if not receipt["restored_and_verified"]:
        raise RuntimeError("firmware restoration did not verify")
    return 0


if __name__ == "__main__":
    sys.exit(main())
