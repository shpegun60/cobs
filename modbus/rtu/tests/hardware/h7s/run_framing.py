#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Measure the RTU framing policy against the default endpoint on NUCLEO-H7S3L8.

For every mode (MODBUS_HW_FRAMER=0 default burst framing, 1 length-prefixed
framing policy) and every baud the harness is built, inspected, flashed and
driven through the smoke, framing and vectors suites; the board's original
internal flash is backed up first and restored, verified and read back last.
Every record carries the image manifest with its source hashes and base
commit, and the session receipt binds the results file to the flashed images.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import glob
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[4]
ELF = REPO / "stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf"
PROGRAMMERS = sorted(glob.glob(
    "C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*/tools/bin/STM32_Programmer_CLI.exe"))
DEFAULT_BAUDS = (1000000, 3000000, 6000000, 10000000)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True, help="ST-LINK serial number")
    parser.add_argument("--output", type=Path, required=True, help="JSONL results file (must not exist)")
    parser.add_argument("--bauds", default=",".join(map(str, DEFAULT_BAUDS)))
    parser.add_argument("--modes", default="0,1", help="MODBUS_HW_FRAMER values, comma separated")
    parser.add_argument("--policy", default="bitwise")
    parser.add_argument("--programmer", default=PROGRAMMERS[-1] if PROGRAMMERS else None)
    parser.add_argument("--bash", default="C:/Program Files/Git/bin/bash.exe")
    args = parser.parse_args()
    bauds = [int(b) for b in args.bauds.split(",")]
    modes = [int(m) for m in args.modes.split(",")]
    assert all(m in (0, 1) for m in modes) and bauds and modes
    assert args.programmer and Path(args.programmer).exists(), "STM32_Programmer_CLI.exe not found"
    output = args.output if args.output.is_absolute() else REPO / args.output
    assert not output.exists(), f"refusing to append to an existing results file: {output}"

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = REPO / f"stm32_cube_test/h7s_cobs_test/out/rtu-framing-{stamp}"
    session.mkdir(parents=True)
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]
    receipt = dict(schema=1, session=str(session), started=stamp, port=args.port, serial=args.serial,
                   policy=args.policy, bauds=bauds, modes=modes, images=[], suites=[],
                   source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                   completed=False, restored_and_verified=False)

    def command(tag, argv, env=None, check=True):
        with (session / f"{tag}.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(argv, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
        if check and completed.returncode != 0:
            raise RuntimeError(f"{tag} failed with exit code {completed.returncode}; see {session / (tag + '.log')}")
        return completed.returncode

    def flash(tag, path, attempts=3):
        """The ST-LINK occasionally refuses one download ("failed to download
        Sector[0]") and succeeds on the next connection; a retry is not a
        weakening because the download is verified afterwards."""
        for attempt in range(1, attempts + 1):
            log = f"{tag}-flash" if attempt == 1 else f"{tag}-flash-retry{attempt}"
            code = command(log, [args.programmer, *connection, "-w", str(path), "-v", "-rst"], check=False)
            text = (session / (log + ".log")).read_text(encoding="utf-8", errors="replace")
            if code == 0 and "Download verified successfully" in text:
                return attempt
            print(f"  flash attempt {attempt} failed (exit {code}); retrying", flush=True)
            time.sleep(2.0)
        raise RuntimeError(f"{tag}: flash failed {attempts} times; see {session}")

    def save():
        (output.parent / (output.name + ".session.json")).write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    backup = session / "before.bin"
    command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert backup.stat().st_size == 65536
    receipt["backup_sha256"] = sha256(backup)
    receipt["backup_bytes"] = 65536
    save()
    try:
        for mode in modes:
            for baud in bauds:
                tag = f"rtu-{args.policy}-{baud}-framer{mode}"
                print(f"BUILD + FLASH {tag}", flush=True)
                environment = os.environ.copy()
                environment.update(MODBUS_HW_BAUD=str(baud), MODBUS_HW_CRC_POLICY=args.policy,
                                   MODBUS_HW_FRAMER=str(mode), MODBUS_HW_OPT="-Os", MODBUS_HW_LTO="0")
                command(tag + "-build", [args.bash, str(HERE / "build.sh")], environment)
                command(tag + "-inspect", [sys.executable, "-B", str(HERE / "inspect_image.py"), str(ELF),
                                           "--policy", args.policy, "--baud", str(baud),
                                           "--optimization", "Os", "--lto", "0", "--framer", str(mode)])
                manifest = ELF.with_suffix(".image.json")
                shutil.copy(ELF, session / (tag + ".elf"))
                shutil.copy(manifest, session / (tag + ".image.json"))
                attempts = flash(tag, ELF)
                receipt["images"].append(dict(mode=mode, baud=baud, tag=tag, elf_sha256=sha256(ELF), flash_attempts=attempts,
                                              binary_sha256=json.loads(manifest.read_text(encoding="utf-8"))["binary_sha256"]))
                save()
                for suite in ("smoke", "framing", "vectors"):
                    argv = [sys.executable, "-B", str(HERE / "modbus_hardware.py"), args.port, "--baud", str(baud),
                            "--crc-policy", args.policy, "--suite", suite, "--image", str(manifest),
                            "--output", str(output)]
                    if mode:
                        argv.append("--framer")
                    # The peer appends its own "failed" record when a suite raises; the
                    # default endpoint is EXPECTED to fail vectors where the ST-Link bridge
                    # splits frames, so the outcome is recorded, not enforced.
                    code = command(f"{tag}-{suite}", argv, check=False)
                    receipt["suites"].append(dict(mode=mode, baud=baud, suite=suite, exit_code=code))
                    print(f"  {suite}: {'passed' if code == 0 else 'FAILED (recorded)'}", flush=True)
                    save()
        receipt["completed"] = True
    finally:
        restored = False
        for attempt in range(1, 4):
            log = "restore" if attempt == 1 else f"restore-retry{attempt}"
            command(log, [args.programmer, *connection, "-w", str(backup), "0x08000000", "-v", "-rst"], check=False)
            restored = "Download verified successfully" in (session / (log + ".log")).read_text(encoding="utf-8", errors="replace")
            if restored:
                break
            time.sleep(2.0)
        after = session / "after.bin"
        command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"], check=False)
        receipt["restored_and_verified"] = restored and after.exists() and sha256(after) == receipt["backup_sha256"]
        receipt["results_sha256"] = sha256(output) if output.exists() else None
        save()
        print(f"RESTORED {receipt['restored_and_verified']}; results {output}", flush=True)


if __name__ == "__main__":
    main()
