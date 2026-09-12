#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Live Heap/Pool + software/peripheral CRC comparison; exclusive board access.

Fresh 64 KiB backup, verified images, no runtime retries, unconditional restore
and full read-back. Raw MCU lines, serial digests and source/image hashes remain
available independently of the human-readable summary.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

import serial
import verify

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
PROJECT = REPO / "stm32_cube_test/h7s_cobs_test"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sources():
    files = set(HERE.glob("*.cpp")) | set(HERE.glob("*.sh")) | {HERE / "run.py", HERE / "verify.py"}
    for name in ("cobs", "modbus", "wire", "crc", "uart", "adapters/cobs", "adapters/rtu", "adapters/stm32"):
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


def collect_lines(port, command, terminal, seconds, record):
    port.write(command.encode("ascii")); port.flush()
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = port.read_until(b"\n", 4096)
        if not line:
            continue
        assert line.endswith(b"\n"), f"partial report: {line!r}"
        text = line.decode("ascii").strip()
        record.append(text)
        if text.startswith(terminal + ","):
            return
    raise TimeoutError(f"no {terminal} report")


def read_frame(port, protocol, policy):
    if protocol == 0:
        return port.read_until(b"\0", 300)
    prefix = port.read(4)
    if len(prefix) != 4:
        raise TimeoutError(f"incomplete RTU header: {prefix.hex()}")
    size = int.from_bytes(prefix[2:4], "big")
    assert size <= 252, f"bad RTU length {size}"
    return prefix + port.read(size + (2 if policy else 0))


def snapshot(port, protocol, policy):
    tx = verify.wire(verify.MAGIC + b"S", protocol, policy, False)
    assert port.write(tx) == len(tx)
    port.flush()
    rx = read_frame(port, protocol, policy)
    words = verify.telemetry(rx, protocol, policy)
    return dict(tx=tx.hex(), rx=rx.hex(), words=list(words))


def uart_runs(port, protocol, policy, seconds, rows, raw_log):
    # Keep a buffered log open; opening a Windows file for every echo would
    # unnecessarily lower the VCP-paced throughput we are observing.
    with raw_log.open("w", encoding="utf-8", newline="\n") as log:
        uart_runs_logged(port, protocol, policy, seconds, rows, log)


def uart_runs_logged(port, protocol, policy, seconds, rows, log):
    lines = []
    collect_lines(port, "U", "UART", 3, lines)
    assert lines == ["UART,1000000"]
    port.baudrate = 1000000
    time.sleep(0.05)
    for memory in (0, 1):
        if memory:
            data = verify.MAGIC + b"M"
            port.write(verify.wire(data, protocol, policy, False)); port.flush()
            assert verify.unwire(read_frame(port, protocol, policy), protocol, policy, False) == data
            time.sleep(0.04)
            if protocol == 0:
                port.write(b"\0"); port.flush(); time.sleep(0.01)
        for case in ("short8", "long250", "mixed"):
            corpus = verify.corpus(case)
            frames = [verify.wire(body, protocol, policy, False) for body in corpus]
            for repetition in range(2):
                before = snapshot(port, protocol, policy)
                assert before["words"][3] == memory
                sent = received = payload_bytes = 0
                observed = hashlib.sha256()
                started = time.monotonic()
                while time.monotonic() - started < seconds:
                    index = sent % len(frames)
                    tx = frames[index]
                    assert port.write(tx) == len(tx); port.flush()
                    sent += 1
                    rx = read_frame(port, protocol, policy)
                    log.write(json.dumps(dict(memory=memory, case=case, repetition=repetition,
                        index=sent - 1, tx=tx.hex(), rx=rx.hex()), sort_keys=True) + "\n")
                    assert rx == tx, f"UART echo mismatch: {memory}/{case}/{sent}"
                    observed.update(rx); received += 1
                    payload_bytes += len(corpus[index])
                elapsed = time.monotonic() - started
                after = snapshot(port, protocol, policy)
                row = dict(memory=memory, case=case, repetition=repetition, requested_seconds=seconds,
                           host_seconds=elapsed, sent=sent, received=received, payload_bytes=payload_bytes,
                           observed_wire_sha256=observed.hexdigest(), before=before, after=after)
                verify.check_uart(row, protocol, policy)
                rows.append(row)
                log.flush()
                print(f"  UART {'Heap' if memory else 'Pool'} {case} #{repetition}: {sent} exact echoes", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--probe", action="store_true", help="hardware COBS image, verify/raw/core only; no UART matrix")
    parser.add_argument("--programmer", default="C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")
    args = parser.parse_args()
    if not __debug__: parser.error("python -O is forbidden")
    assert 1 <= args.seconds <= 10
    output = args.output.resolve()
    if output.exists(): parser.error("refusing to overwrite evidence")
    output.mkdir(parents=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = PROJECT / "out" / ("heap-crc-" + stamp)
    session.mkdir()
    plan = [(0, 3)] if args.probe else [(p, c) for c in (3, 0, 1, 2) for p in (0, 1)]
    receipt = dict(schema=1, plan=plan, probe=args.probe, seconds=args.seconds, images=[], completed=False,
                   restored_and_verified=False, session=str(session), started=stamp, port=args.port, serial=args.serial,
                   source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip())
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]

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
        raise RuntimeError(f"flash failed: {tag}")

    backup = session / "before.bin"
    code, _ = command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert code == 0 and backup.stat().st_size == 65536
    receipt.update(backup_bytes=65536, backup_sha256=digest(backup)); save()
    try:
        for protocol, policy in plan:
            tag = f"{protocol}-{policy}"
            print(f"BUILD/FLASH Heap/CRC {tag}", flush=True)
            before = sources()
            env = os.environ.copy(); env.update(HEAP_CRC_PROTOCOL=str(protocol), HEAP_CRC_POLICY=str(policy))
            code, log = command("build-" + tag, ["C:/Program Files/Git/bin/bash.exe", str(HERE / "build.sh")], env)
            assert code == 0, f"build failed: {log}"
            assert sources() == before, "inputs changed during build"
            build = PROJECT / "out" / ("heap-crc-" + tag)
            for suffix in ("elf", "bin", "map", "dis", "nm"):
                shutil.copy2(build / ("bench." + suffix), session / (tag + "." + suffix))
            elf, binary = session / (tag + ".elf"), session / (tag + ".bin")
            assert binary.stat().st_size <= 65536
            image = dict(tag=tag, protocol=protocol, policy=policy, elf_sha256=digest(elf), binary_sha256=digest(binary),
                         binary_bytes=binary.stat().st_size, build_log_sha256=digest(log), source_sha256=before,
                         artifact_sha256={suffix: digest(session / (tag + "." + suffix))
                                          for suffix in ("elf", "bin", "map", "dis", "nm")},
                         lines=[], uart=[])
            image["flash"] = flash("flash-" + tag, elf)
            receipt["images"].append(image); save()
            time.sleep(0.5)
            with serial.Serial(args.port, 115200, timeout=0.5, write_timeout=3) as port:
                port.reset_input_buffer()
                for key, terminal in (("H", "HELLO"), ("V", "VERIFY"), ("C", "ENDCRC"), ("B", "ENDCORE")):
                    try:
                        collect_lines(port, key, terminal, 120, image["lines"])
                    finally:
                        save()
                verify.check_lines(image)
                print(f"  PASS core and CRC oracle: {tag}", flush=True)
                if not args.probe:
                    raw_log = session / (tag + "-transfers.jsonl")
                    try:
                        uart_runs(port, protocol, policy, args.seconds, image["uart"], raw_log)
                    finally:
                        if raw_log.exists(): image["transfers_sha256"] = digest(raw_log)
                        save()
            assert sources() == before, "inputs changed during live measurement"
            save()
        receipt["completed"] = True
    except BaseException as exc:
        receipt["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        try:
            receipt["restore_flash"] = flash("restore", backup, binary=True)
            after = session / "after.bin"
            code, _ = command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"])
            receipt["readback_sha256"] = digest(after) if after.exists() else None
            receipt["restored_and_verified"] = code == 0 and receipt["readback_sha256"] == receipt["backup_sha256"]
        finally:
            save(); print(f"RESTORED_AND_VERIFIED={receipt['restored_and_verified']}", flush=True)
    assert receipt["restored_and_verified"]
    return subprocess.run([sys.executable, "-B", str(HERE / "verify.py"), str(output), "--local-images"], cwd=REPO).returncode


if __name__ == "__main__":
    sys.exit(main())
