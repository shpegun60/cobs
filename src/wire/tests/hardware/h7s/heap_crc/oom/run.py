#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Exclusive live OOM diagnosis, with original-flash backup/restore/read-back."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
import serial
import verify

HERE, REPO = verify.HERE, verify.REPO
PROJECT = REPO / "stm32_cube_test/h7s_cobs_test"


def sources():
    files = {p for p in HERE.iterdir() if p.suffix in (".cpp", ".sh", ".py")}
    files.update((HERE.parent / "heap.cpp", REPO / "src/cobs/Encoder.cpp", REPO / "src/cobs/Decoder.cpp",
                  REPO / "src/uart/tests/bench/uart_bench.h", PROJECT / "Boot/STM32H7S3L8HX_FLASH.ld"))
    for name in ("cobs", "modbus", "wire", "crc", "uart"):
        root = REPO / "src" / name
        files.update(p for p in root.rglob("*.h") if "tests" not in p.relative_to(root).parts)
    for name in ("delegate", "spsc"):
        root = REPO / "libs" / name
        for pattern in ("*.h", "*.hpp"):
            files.update(p for p in root.rglob(pattern) if "tests" not in p.relative_to(root).parts)
    for root in (PROJECT / "Boot/Core", PROJECT / "Drivers"):
        for pattern in ("*.h", "*.c", "*.s"):
            files.update(root.rglob(pattern))
    return {p.relative_to(REPO).as_posix(): verify.digest(p) for p in sorted(files)}


def line(port, seconds=3):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        raw = port.read_until(b"\n", 512)
        if raw:
            assert raw.endswith(b"\n"), f"partial report {raw!r}"
            return raw.decode("ascii").strip()
    raise TimeoutError("MCU did not report an observation")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--programmer", default="C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    output = args.output.resolve()
    if output.exists(): parser.error("refusing to overwrite existing evidence")
    output.mkdir(parents=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = PROJECT / "out" / ("heap-oom-" + stamp)
    session.mkdir()
    record = dict(schema=1, started=stamp, session=str(session), port=args.port, serial=args.serial,
                  plan=list(verify.PLAN), cases=[], completed=False, restored_and_verified=False,
                  source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                  source_sha256=sources())
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]

    def save():
        (output / "session.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def command(tag, argv, timeout=60):
        log = session / (tag + ".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(argv, cwd=REPO, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        return result.returncode, log

    def flash(tag, path, binary=False):
        for attempt in range(1, 4):
            code, log = command(f"{tag}-{attempt}", [args.programmer, *connection, "-w", str(path),
                *(["0x08000000"] if binary else []), "-v", "-rst"])
            if code == 0 and "Download verified successfully" in log.read_text(errors="replace"):
                return dict(attempts=attempt, log_sha256=verify.digest(log))
        raise RuntimeError(f"flash failed: {tag}")

    save()
    code, log = command("build", ["C:/Program Files/Git/bin/bash.exe", str(HERE / "build.sh")], timeout=180)
    assert code == 0, f"build failed: {log}"
    assert sources() == record["source_sha256"], "inputs changed during build"
    record["build_log_sha256"] = verify.digest(log)
    build = PROJECT / "out/heap-oom"
    for suffix in ("elf", "bin", "map", "dis", "nm"):
        shutil.copy2(build / ("bench." + suffix), session / ("bench." + suffix))
    record["artifact_sha256"] = {suffix: verify.digest(session / ("bench." + suffix)) for suffix in ("elf", "bin", "map", "dis", "nm")}
    record["binary_bytes"] = (session / "bench.bin").stat().st_size
    assert record["binary_bytes"] <= 65536
    disassembly = (session / "bench.dis").read_text()
    verify.check_disassembly(disassembly) # reject a different runtime before flashing
    excerpt = "".join(block for block in re.split(r"(?m)(?=^[0-9a-f]+ <)", disassembly)
        if re.match(r"^[0-9a-f]+ <(?:operator new\(|std::get_new_handler\(|__wrap_abort>|__wrap__exit>|abort>|_exit>)", block))
    (output / "allocator_path.dis").write_text(excerpt, encoding="utf-8", newline="\n")
    record["allocator_path_sha256"] = verify.digest(output / "allocator_path.dis")
    save()
    backup = session / "before.bin"
    code, log = command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert code == 0 and backup.stat().st_size == 65536
    record.update(backup_bytes=65536, backup_sha256=verify.digest(backup), backup_log_sha256=verify.digest(log)); save()
    try:
        record["flash"] = flash("flash", session / "bench.elf"); save()
        for index, key in enumerate(verify.PLAN):
            if index:
                code, log = command(f"reset-{index}-{key}", [args.programmer, *connection, "-rst"])
                assert code == 0, f"reset failed: {log}"
            time.sleep(0.25)
            with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=3) as port:
                port.reset_input_buffer(); port.write(b"H"); port.flush()
                row = dict(command=key, hello=line(port), lines=[], after_ping=None)
                record["cases"].append(row); save()
                assert row["hello"].startswith("HELLO,1,600000000,")
                port.write(key.encode("ascii")); port.flush()
                for _ in range(8):
                    report = line(port)
                    row["lines"].append(report); save()
                    if report.startswith(("EXIT,", "RECOVERED,", "RETURNED_")): break
                port.write(b"H"); port.flush()
                if key in verify.FATAL:
                    row["after_ping"] = port.read_until(b"\n", 512).decode("ascii").strip()
                else:
                    row["after_ping"] = line(port)
                save(); verify.check_case(row)
                print(f"{key}: {'CONFIRMED abort -> _exit' if key in verify.FATAL else 'NULL/Pool control and recovery confirmed'}", flush=True)
        assert sources() == record["source_sha256"], "inputs changed during live test"
        record["completed"] = True
    except BaseException as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        try:
            record["restore"] = flash("restore", backup, binary=True)
            after = session / "after.bin"
            code, log = command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"])
            record["readback_sha256"] = verify.digest(after) if after.exists() else None
            record["readback_log_sha256"] = verify.digest(log)
            record["restored_and_verified"] = code == 0 and record["readback_sha256"] == record["backup_sha256"]
        finally:
            save(); print(f"RESTORED_AND_VERIFIED={record['restored_and_verified']}", flush=True)
    assert record["restored_and_verified"]
    verify.verify(output, local=True)


if __name__ == "__main__": main()
