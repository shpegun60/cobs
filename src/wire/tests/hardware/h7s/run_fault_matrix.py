#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Repeat COBS/RTU hardware suites, preserving the board's original boot flash.

The existing protocol runners supply every test and its acceptance criteria.
One image per invocation lets this wrapper retain each ELF and bind its rows
to the flash log. Qt interoperability and RTU framing have separate runners.
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


HERE = Path(__file__).resolve().parent
REPO = next(p for p in (HERE, *HERE.parents) if (p / "COBS.pro").is_file())
SRC = REPO / "src"
PROGRAMMER = "C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe"
COBS_CONFIGS = (("bitwise", 253), ("table", 253), ("none", 1024))
COBS_BAUDS = (115200, 1000000, 3000000, 6000000, 10000000)
RTU_POLICIES = ("nocrc", "crc8-bitwise", "crc8-table", "crc16-bitwise", "crc16-table",
                "crc32-bitwise", "crc32-table", "crc64-bitwise", "crc64-table")
RTU_BAUDS = (115200, 1000000)


def plan():
    phases = []
    for policy, maximum in COBS_CONFIGS:
        for baud in COBS_BAUDS:
            phases.append(dict(tag=f"cobs-{policy}-{maximum}-{baud}", protocol="cobs", policy=policy,
                               baud=baud, maximum=maximum, seconds=10,
                               extended_seconds=30 if baud == 10000000 else 0))
    for policy in RTU_POLICIES:
        for baud in RTU_BAUDS:
            phases.append(dict(tag=f"rtu-{policy}-{baud}", protocol="rtu", policy=policy,
                               baud=baud, seconds=5, extended_seconds=15 if baud == 1000000 else 0))
    return phases


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def quote_ps(value):
    return "'" + str(value).replace("'", "''") + "'"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory for raw JSONL files and receipt")
    parser.add_argument("--programmer", default=PROGRAMMER)
    parser.add_argument("--dry-run", action="store_true", help="print the matrix without creating files or touching hardware")
    args = parser.parse_args()
    if not re.fullmatch(r"COM[0-9]+", args.port) or not re.fullmatch(r"[A-Za-z0-9]+", args.serial):
        parser.error("expected a COM port and an alphanumeric ST-LINK serial")
    phases = plan()
    if args.dry_run:
        print(json.dumps(phases, indent=2))
        return 0
    output = args.output.resolve()
    if output.exists():
        parser.error(f"refusing to reuse an existing output directory: {output}")
    if not Path(args.programmer).is_file():
        parser.error(f"programmer not found: {args.programmer}")
    output.mkdir(parents=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = SRC / "wire/tests/out" / f"fault-matrix-{stamp}"
    session.mkdir(parents=True)
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]
    receipt = dict(schema=1, started=stamp, port=args.port, serial=args.serial, session=str(session),
                   source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                   runner_sha256=digest(__file__), plan=phases, phases=[], completed=False,
                   restored_and_verified=False)

    def save():
        (output / "session.json").write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def command(tag, argv, env=None, timeout=600):
        log = session / f"{tag}.log"
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(argv, cwd=REPO, env=env, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        return result.returncode, log

    backup = session / "before.bin"
    code, _ = command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"], timeout=60)
    if code or not backup.is_file() or backup.stat().st_size != 65536:
        raise RuntimeError(f"valid 64 KiB backup required before flashing; see {session}")
    receipt.update(backup_sha256=digest(backup), backup_bytes=65536)
    save()
    environment = os.environ.copy()
    environment.update(MODBUS_HW_FRAMER="0", MODBUS_HW_ROLE="0", MODBUS_HW_CXXFLAGS_EXTRA="",
                       COBS_HW_UART_CHUNK_SIZE="128", COBS_HW_UART_CHUNK_COUNT="8")
    try:
        for phase in phases:
            protocol, tag = phase["protocol"], phase["tag"]
            hardware = SRC / ("cobs" if protocol == "cobs" else "modbus/rtu") / "tests/hardware/h7s"
            results = output / (tag + ".jsonl")
            words = ["&", quote_ps(hardware / "run_matrix.ps1"), "-Port", quote_ps(args.port),
                     "-StLinkSerial", quote_ps(args.serial), "-CubeProgrammer", quote_ps(args.programmer),
                     "-BaudRates", str(phase["baud"]), "-StressSeconds", str(phase["seconds"]),
                     "-ExtendedSeconds", str(phase["extended_seconds"]), "-Output", quote_ps(results), "-LeaveAtLastBaud"]
            if protocol == "cobs":
                words += ["-Crc", quote_ps(phase["policy"]), "-MaxPayload", str(phase["maximum"])]
            else:
                words += ["-CrcPolicies", quote_ps(phase["policy"])]
            print(f"RUN {tag}", flush=True)
            entry = dict(tag=tag, results=results.name, completed=False)
            receipt["phases"].append(entry)
            save()
            code, log = command(tag, ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                      "-Command", " ".join(words)], environment)
            entry.update(exit_code=code, log_sha256=digest(log))
            elf_name = "cobs_hardware_bench" if protocol == "cobs" else "modbus_hardware_bench"
            folder = "cobs-hardware" if protocol == "cobs" else "modbus-hardware"
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out" / folder / (elf_name + ".elf")
            if elf.exists():
                snapshot = session / (tag + ".elf")
                shutil.copy2(elf, snapshot)
                entry["elf_sha256"] = digest(snapshot)
                for suffix in (".map", ".bin", ".image.json"):
                    candidate = elf.with_suffix(suffix)
                    if candidate.exists():
                        shutil.copy2(candidate, session / (tag + suffix))
            if results.exists():
                entry["results_sha256"] = digest(results)
            entry["flash_verified"] = "Download verified successfully" in log.read_text(encoding="utf-8", errors="replace")
            entry["completed"] = code == 0 and entry["flash_verified"] and results.exists()
            save()
            if not entry["completed"]:
                raise RuntimeError(f"{tag} failed (exit {code}); preserved log: {log}")
            print(f"PASS {tag}", flush=True)
        receipt["completed"] = True
    except BaseException as error:
        receipt["error"] = repr(error)
        raise
    finally:
        # Restore even if a child runner failed or left a high-baud harness.
        restored = False
        restore_errors = []
        for attempt in range(1, 4):
            try:
                code, log = command(f"restore-{attempt}", [args.programmer, *connection,
                                    "-w", str(backup), "0x08000000", "-v", "-rst"], timeout=60)
                restored = code == 0 and "Download verified successfully" in log.read_text(encoding="utf-8", errors="replace")
                if restored:
                    break
            except Exception as error:
                restore_errors.append(repr(error))
            time.sleep(1)
        try:
            after = session / "after.bin"
            code, _ = command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"], timeout=60)
            if code == 0 and after.exists():
                receipt["readback_sha256"] = digest(after)
                receipt["restored_and_verified"] = restored and digest(after) == receipt["backup_sha256"]
        except Exception as error:
            restore_errors.append(repr(error))
        receipt["restore_errors"] = restore_errors
        receipt["finished"] = datetime.now(timezone.utc).isoformat()
        save()
        print(f"RESTORED {receipt['restored_and_verified']}; results {output}", flush=True)
    if not receipt["restored_and_verified"]:
        raise RuntimeError(f"original firmware restoration NOT verified; backup remains at {backup}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
