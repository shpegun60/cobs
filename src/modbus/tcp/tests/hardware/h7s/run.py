#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Exclusive H7S MBAP-over-UART test with fresh flash backup and finally restoration."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import time
import serial
import verify

HERE, REPO = verify.HERE, verify.REPO
PROJECT = REPO / "stm32_cube_test/h7s_cobs_test"


def sources():
    files = set()
    for root in (REPO / "src/modbus/tcp", REPO / "src/modbus/rtu", REPO / "src/wire", REPO / "src/crc", REPO / "src/uart",
                 REPO / "libs/delegate", REPO / "libs/spsc", PROJECT / "Boot/Core", PROJECT / "Drivers"):
        files.update(p for p in root.rglob("*") if p.suffix in (".h", ".hpp", ".cpp", ".c", ".sh", ".py", ".s")
                     and "out" not in p.relative_to(root).parts and "results_2026-09-12" not in p.parts)
    files.update((REPO / "src/modbus/Pdu.h", REPO / "src/modbus/Types.h",
                  PROJECT / "Boot/STM32H7S3L8HX_FLASH.ld"))
    return {p.relative_to(REPO).as_posix(): verify.digest(p) for p in sorted(files)}


def line(port, timeout=15):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        data.extend(port.read_until(b"\n", 256))
        if data.endswith(b"\n"):
            return data.decode("ascii").strip()
    raise TimeoutError(f"MCU report missing/partial: {data!r}")


def read_exact(port, count, timeout=3):
    deadline = time.monotonic() + timeout
    result = bytearray()
    while len(result) < count and time.monotonic() < deadline:
        result.extend(port.read(count - len(result)))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--programmer", default="C:/ST/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI.exe")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    output = args.output.resolve()
    if output.exists(): parser.error("refusing to overwrite evidence")
    output.parent.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = PROJECT / "out" / ("tcp-live-" + stamp)
    session.mkdir()
    record = dict(schema=2, limit_kind="function-data bytes", max_data_size=1024,
                  started=stamp, session=str(session), port=args.port, serial=args.serial,
                  transport="UART byte transport; no TCP/IP stack", completed=False, restored_and_verified=False,
                  images=[], log_sha256={}, source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                  source_sha256=sources())
    connection = ["-vb", "3", "-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]

    def save():
        output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    def command(tag, argv, timeout=60):
        log = session / (tag + ".log")
        with log.open("w", encoding="utf-8") as stream:
            result = subprocess.run(argv, cwd=REPO, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        record["log_sha256"][log.name] = verify.digest(log)
        save()
        return result.returncode, log

    def flash(tag, path, binary=False):
        for attempt in range(1, 4):
            code, log = command(f"{tag}-{attempt}", [args.programmer, *connection, "-w", str(path),
                *(["0x08000000"] if binary else []), "-v", "-rst"])
            if code == 0 and "Download verified successfully" in log.read_text(errors="replace"):
                return attempt
        raise RuntimeError(f"flash failed: {tag}")

    def boot(tag):
        code, log = command(tag, [args.programmer, *connection, "-rst"])
        assert code == 0, f"reset failed: {log}"
        time.sleep(0.15)

    def open_boot():
        port = serial.Serial(args.port, 115200, timeout=0.1, write_timeout=3)
        try:
            port.reset_input_buffer()
            port.write(b"H"); port.flush()
            return port, line(port)
        except BaseException:
            port.close()
            raise

    def start_echo(port):
        port.write(b"E"); port.flush()
        assert line(port) == "ECHO,115200,DATA,1024"

    save()
    print("Building six MCU images before touching flash...", flush=True)
    code, log = command("build", ["C:/Program Files/Git/bin/bash.exe", str(HERE / "build.sh")], timeout=240)
    assert code == 0, f"build failed: {log}"
    assert sources() == record["source_sha256"], "inputs changed during build"
    for config in verify.CONFIGS:
        shutil.copytree(PROJECT / "out/tcp-core" / config, session / config)
        assert (session / config / "bench.bin").stat().st_size <= 65536
    backup = session / "before.bin"
    code, log = command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert code == 0 and backup.stat().st_size == 65536
    record.update(backup_bytes=65536, backup_sha256=verify.digest(backup)); save()
    try:
        for config in verify.CONFIGS:
            root = session / config
            row = dict(config=config, positive=[], negative=[],
                       binary_bytes=(root / "bench.bin").stat().st_size,
                       artifacts={suffix: verify.digest(root / ("bench." + suffix)) for suffix in ("elf", "bin", "map", "dis", "nm")})
            record["images"].append(row); save()
            row["flash_attempts"] = flash("flash-" + config, root / "bench.elf")
            time.sleep(0.15)
            port, row["hello"] = open_boot()
            with port:
                port.write(b"S"); port.flush()
                row["self_test"] = line(port); save()
                fields = row["self_test"].split(",")
                assert len(fields) == 7 and fields[0] == "SELF" and fields[2] == "0" and fields[4] == "0" and fields[6] == "0", row["self_test"]
                print(f"{config}: {row['self_test']}", flush=True)
                start_echo(port)
                policy = int(config[-1])
                for name, chunks, expected in verify.positive_cases(policy, record["schema"]):
                    observation = dict(name=name, chunks=[b.hex() for b in chunks], received="")
                    row["positive"].append(observation)
                    for part in chunks:
                        port.write(part); port.flush()
                        if len(chunks) > 1: time.sleep(0.004)
                    actual = read_exact(port, len(expected))
                    observation["received"] = actual.hex()
                    assert actual == expected, (config, name, actual.hex(), expected.hex())
                save()
            print(f"{config}: {len(row['positive'])} exact MBAP UART exchanges passed", flush=True)
            for index, (name, sent) in enumerate(verify.negative_cases(policy, record["schema"])):
                boot(f"reset-{config}-{index}")
                port, hello = open_boot()
                assert hello == row["hello"]
                with port:
                    start_echo(port)
                    baseline = verify.frame(policy, 3, 1, 3, b"\x00\x01")
                    port.write(baseline); port.flush()
                    observation = dict(name=name, sent=sent.hex(), baseline=read_exact(port, len(baseline)).hex())
                    row["negative"].append(observation)
                    assert observation["baseline"] == baseline.hex()
                    port.write(sent); port.flush()
                    observation["received"] = read_exact(port, 1, timeout=0.4).hex()
                    assert observation["received"] == "", (config, name)
                boot(f"recover-{config}-{index}")
                port, observation["boot_after"] = open_boot()
                port.close()
                assert observation["boot_after"] == row["hello"]
                save()
            print(f"{config}: {len(row['negative'])} live fail-closed trials passed", flush=True)
        assert sources() == record["source_sha256"], "inputs changed during live test"
        record["completed"] = True
    except BaseException as exc:
        record["error"] = f"{type(exc).__name__}: {exc}"
        raise
    finally:
        try:
            record["restore_attempts"] = flash("restore", backup, binary=True)
            after = session / "after.bin"
            code, log = command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"])
            record["readback_sha256"] = verify.digest(after) if after.exists() else None
            record["restored_and_verified"] = code == 0 and record["readback_sha256"] == record["backup_sha256"]
        finally:
            save(); print(f"RESTORED_AND_VERIFIED={record['restored_and_verified']}", flush=True)
    assert record["restored_and_verified"]
    verify.verify(output, local=True)


if __name__ == "__main__": main()
