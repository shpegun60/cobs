#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Live COBS/RTU public API + real FreeRTOS task notification matrix.

Exclusive ST-Link/COM ownership required. Fresh 64 KiB backup, verified flash,
no runtime retries, unconditional restore and read-back comparison.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

import serial

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
PROJECT = REPO / "stm32_cube_test/h7s_cobs_test"
KERNEL = Path("C:/Users/admin/STM32Cube/Repository/STM32Cube_FW_H7RS_V1.3.0/Middlewares/Third_Party/FreeRTOS/Source")
MAGIC = bytes.fromhex("b6505254")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def checksum(data, policy):
    if not policy:
        return b""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc.to_bytes(2, "little")


def encode(body, protocol, policy, corrupt=False):
    raw = body if protocol == 0 else b"\x11\x41" + len(body).to_bytes(2, "big") + body
    trailer = checksum(raw, policy)
    if corrupt:
        assert policy and body
        raw = raw[:-1] + bytes([raw[-1] ^ 1])  # preserve length, keep the old checksum
    raw += trailer
    if protocol:
        return raw
    raw = bytes([len(raw)]) + raw
    result = bytearray([0])
    position, code = 0, 1
    full = False
    for byte in raw:
        if byte:
            result.append(byte)
            code += 1
            if code != 255:
                continue
        result[position] = code
        full = code == 255
        position = len(result)
        result.append(0)
        code = 1
    if code == 1 and full:
        result.pop()
    else:
        result[position] = code
    return bytes(result) + b"\0"


def read_frame(port, protocol, policy):
    if protocol == 0:
        return port.read_until(b"\0", 300)
    prefix = port.read(4)
    if not prefix:
        return b""
    if len(prefix) != 4:
        raise TimeoutError(f"short RTU prefix {prefix.hex()}")
    length = int.from_bytes(prefix[2:4], "big")
    assert length <= 252, f"impossible RTU body size {length}"
    return prefix + port.read(length + (2 if policy else 0))


def sources():
    files = {HERE / name for name in ("run.py", "build.sh", "parity_bench.cpp", "FreeRTOSConfig.h")}
    for name in ("uart", "cobs", "modbus", "wire", "crc", "adapters/cobs", "adapters/rtu", "adapters/freertos"):
        root = REPO / "src" / name
        files.update(p for p in root.rglob("*.h") if "tests" not in p.relative_to(root).parts)
    files.update(REPO / "src/cobs" / name for name in ("Encoder.cpp", "Decoder.cpp"))
    files.add(REPO / "src/uart/tests/bench/uart_bench.h")
    for name in ("delegate", "spsc"):
        root = REPO / "libs" / name
        for pattern in ("*.h", "*.hpp"):
            files.update(p for p in root.rglob(pattern) if "tests" not in p.relative_to(root).parts)
    for root in (PROJECT / "Boot/Core", PROJECT / "Drivers"):
        for pattern in ("*.h", "*.c", "*.s"):
            files.update(root.rglob(pattern))
    files.add(PROJECT / "Boot/STM32H7S3L8HX_FLASH.ld")
    return {p.relative_to(REPO).as_posix(): digest(p) for p in sorted(files)}


def kernel_sources():
    files = set((KERNEL / "include").glob("*.h"))
    files.update(KERNEL / name for name in ("tasks.c", "list.c", "queue.c"))
    files.update((KERNEL / "portable/GCC/ARM_CM7/r0p1").glob("*.[ch]"))
    return {p.relative_to(KERNEL).as_posix(): digest(p) for p in sorted(files)}


def trial_plan(protocol, policy):
    maximum = (255 if protocol == 0 else 252) - (2 if policy else 0)
    plan = [("hello", MAGIC + b"H", "status")]
    for size in (0, 1, 4, 31, 32, 63, 127, maximum):
        plan.append((f"vector-{size}", bytes((i * 37 + size) & 255 for i in range(size)), "echo"))
    plan += [("busy", MAGIC + b"B", "status"), ("busy-check", MAGIC + b"S", "status")]
    if policy:
        plan += [("bad-crc", b"intentionally-corrupt", "bad-crc"), ("crc-recovery", MAGIC + b"C", "status")]
    if protocol != 1:
        plan += [("detach", MAGIC + b"D", "status"), ("partial-detach", bytes(range(100)), "partial")]
        if protocol == 0:
            plan.append(("resync", b"", "resync"))
        plan.append(("detach-recovery", MAGIC + b"R", "status"))
    if protocol == 2:
        plan += [("orphan", bytes(range(100)), "orphan"), ("deadline-recovery", MAGIC + b"T", "status")]
    for index in range(32):
        plan.append((f"stress-{index}", bytes((index + i * 13) & 255 for i in range(80 + index)), "echo"))
    plan += [("before-idle", MAGIC + b"W", "status"), ("after-idle", MAGIC + b"I", "status")]
    return plan


def exercise(port, protocol, policy, baud, image, output):
    # Acceptance is independently re-derived from these raw exchanges by verify.py.
    for name, body, kind in trial_plan(protocol, policy):
        row = dict(name=name, protocol=protocol, policy=policy, baud=baud, image_sha256=image,
                   kind=kind, body=body.hex(), tx="", rx="", status="running")
        started = time.monotonic()
        try:
            if name == "after-idle":
                time.sleep(0.16)
            tx = encode(body, protocol, policy, corrupt=kind == "bad-crc")
            if kind in ("partial", "orphan"):
                tx = tx[:len(tx) // 2]
            if kind == "resync":
                tx = b"\0"
            row["tx"] = tx.hex()
            assert port.write(tx) == len(tx), "short serial write"
            port.flush()
            if kind in ("resync", "orphan"):
                time.sleep(0.025)
                assert port.in_waiting == 0, "partial/sync input published a packet"
            else:
                row["rx"] = read_frame(port, protocol, policy).hex()
                if kind == "bad-crc":
                    assert not row["rx"], "corrupt CRC accepted"
                elif kind == "echo":
                    assert row["rx"] == encode(body, protocol, policy).hex(), f"echo mismatch: {name}"
                else:
                    assert row["rx"], f"no status: {name}"
            row["status"] = "passed"
        except BaseException as exc:
            row.update(status="failed", error=f"{type(exc).__name__}: {exc}")
            raise
        finally:
            row["elapsed_seconds"] = time.monotonic() - started
            with output.open("a", encoding="utf-8", newline="\n") as stream:
                stream.write(json.dumps(row, sort_keys=True) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--bauds", default="115200,1000000")
    parser.add_argument("--programmer", default="C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")
    args = parser.parse_args()
    if not __debug__:
        parser.error("python -O is not permitted")
    bauds = [int(value) for value in args.bauds.split(",")]
    assert bauds and len(set(bauds)) == len(bauds) and all(b in (115200, 1000000) for b in bauds)
    assert checksum(b"123456789", 1) == bytes.fromhex("374b")
    assert encode(b"AB", 0, 1) == bytes.fromhex("06044142b1d100")
    output = args.output.resolve()
    if output.exists():
        parser.error("refusing to overwrite evidence")
    output.mkdir(parents=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = PROJECT / "out" / ("api-rtos-" + stamp)
    session.mkdir()
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]
    plan = [(p, c, b) for p, c in ((0, 0), (0, 1), (0, 2), (1, 1), (2, 0), (2, 1), (2, 2)) for b in bauds]
    receipt = dict(schema=1, started=stamp, session=str(session), port=args.port, serial=args.serial,
                   plan=plan, images=[], completed=False, restored_and_verified=False,
                   source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                   kernel_root=str(KERNEL), kernel_version="V10.6.2", kernel_sha256=kernel_sources())

    def save():
        (output / "session.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def command(tag, argv, env=None, timeout=180):
        log = session / (tag + ".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(argv, cwd=REPO, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        return result.returncode, log

    def flash(tag, path, binary=False):
        for attempt in range(1, 4):
            code, log = command(f"{tag}-{attempt}", [args.programmer, *connection, "-w", str(path),
                                *(["0x08000000"] if binary else []), "-v", "-rst"], timeout=60)
            if code == 0 and "Download verified successfully" in log.read_text(errors="replace"):
                return dict(attempts=attempt, log_sha256=digest(log))
        raise RuntimeError(f"verified flash failed: {tag}, see {session}")

    backup = session / "before.bin"
    code, _ = command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert code == 0 and backup.stat().st_size == 65536, "fresh full backup required"
    receipt.update(backup_bytes=65536, backup_sha256=digest(backup))
    save()
    try:
        for protocol, policy, baud in plan:
            tag = f"{protocol}-{policy}-{baud}"
            print(f"BUILD/FLASH real FreeRTOS {tag}", flush=True)
            before = sources()
            env = os.environ.copy()
            env.update(PARITY_PROTOCOL=str(protocol), PARITY_CRC=str(policy), PARITY_BAUD=str(baud))
            code, log = command("build-" + tag, ["C:/Program Files/Git/bin/bash.exe", str(HERE / "build.sh")], env)
            assert code == 0, f"build failed: {log}"
            assert sources() == before and kernel_sources() == receipt["kernel_sha256"], "build inputs changed"
            build = PROJECT / "out" / ("api-rtos-" + tag)
            for suffix in ("elf", "bin", "map", "dis"):
                shutil.copy2(build / ("parity_bench." + suffix), session / (tag + "." + suffix))
            elf, binary = session / (tag + ".elf"), session / (tag + ".bin")
            assert binary.stat().st_size <= 65536
            image = dict(tag=tag, protocol=protocol, policy=policy, baud=baud,
                         elf_sha256=digest(elf), binary_sha256=digest(binary), binary_bytes=binary.stat().st_size,
                         source_sha256=before, build_log_sha256=digest(log))
            image["flash"] = flash("flash-" + tag, elf)
            receipt["images"].append(image)
            save()
            time.sleep(0.6)
            with serial.Serial(args.port, baud, timeout=0.5, write_timeout=2) as port:
                port.reset_input_buffer()
                exercise(port, protocol, policy, baud, image["elf_sha256"], output / "results.jsonl")
            # Do not wait until matrix end to reject device-side assertions.
            import verify
            verify.check_rows([json.loads(line) for line in (output / "results.jsonl").read_text().splitlines()], receipt["images"])
            print(f"PASS real FreeRTOS {tag}", flush=True)
        receipt["completed"] = True
    except BaseException as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        try:
            receipt["restore_flash"] = flash("restore", backup, binary=True)
            after = session / "after.bin"
            code, _ = command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"])
            receipt["readback_sha256"] = digest(after) if after.is_file() else None
            receipt["restored_and_verified"] = code == 0 and receipt["readback_sha256"] == receipt["backup_sha256"]
        finally:
            if (output / "results.jsonl").exists():
                receipt["results_sha256"] = digest(output / "results.jsonl")
            save()
            print(f"RESTORED_AND_VERIFIED={receipt['restored_and_verified']}", flush=True)
    assert receipt["restored_and_verified"], "firmware restoration failed"
    return subprocess.run([sys.executable, "-B", str(HERE / "verify.py"), str(output), "--local-images"], cwd=REPO).returncode


if __name__ == "__main__":
    sys.exit(main())
