#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Live paired COBS/RTU benchmarks, with exact firmware backup/restoration."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[4]
HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(REPO / "cobs/tests/hardware/h7s"), str(REPO / "modbus/rtu/tests/hardware/h7s")]
import cobs_hardware as cobs
import modbus_hardware as rtu
import serial

POLICIES = ("none", "bitwise", "table")
PATTERNS = ("random", "zero", "nonzero", "alternating", "boundary")
SIZES = (8, 32, 128, 252, 1024)
CASES = (("random8", (8,), 0), ("random32", (32,), 0),
         ("random128", (128,), 0), ("random252", (252,), 0),
         ("zero252", (252,), 1), ("nonzero252", (252,), 2),
         ("mixed", (8, 32, 128, 252), -1))


def payload(size, pattern):
    state = 0xC0B50000 ^ size
    result = bytearray()
    for i in range(size):
        state = (state ^ (state << 13)) & 0xFFFFFFFF
        state ^= state >> 17
        state = (state ^ (state << 5)) & 0xFFFFFFFF
        value = state & 255
        if pattern == 1: value = 0
        if pattern == 2: value = 1 + i % 255
        if pattern == 3: value = 0 if i % 2 == 0 else 0xA5
        if pattern == 4: value = 0 if i % 254 in (0, 253) else (i * 17 + 3) & 255
        result.append(value)
    return bytes(result)


def policy_for(name):
    return rtu.CRC_POLICIES["nocrc" if name == "none" else f"crc16-{name}"]


def wire(protocol, policy, body, maximum=253):
    if protocol == "rtu":
        # Permit the explicitly labelled private wide geometry too; keep the
        # independent polynomial oracle, not the standard-256 make_adu guard.
        return policy_for(policy).append(b"\x11\x41" + body)
    checksum = b"" if policy == "none" else policy_for(policy).append(body)[len(body):]
    width = 1 if maximum + len(checksum) <= 255 else 2
    raw = (len(body) + len(checksum)).to_bytes(width, "little") + body + checksum
    return cobs.cobs_encode(raw) + b"\0"


def core_group(fields, policy):
    proto_id, policy_id, maximum, size, pattern, chunk, wire_size = map(int, fields[1:8])
    protocol = ("cobs", "rtu")[proto_id]
    assert policy_id == POLICIES.index(policy)
    assert size in SIZES and 0 <= pattern < 5
    assert chunk in ((0, 128) if protocol == "cobs" else (0,))
    expected_max = 1024 if size == 1024 else (
        (255 if policy == "none" else 253) if protocol == "cobs" else (254 if policy == "none" else 252))
    assert maximum == expected_max
    body = payload(size, pattern)
    expected = wire(protocol, policy, body, maximum)
    assert len(expected) == wire_size and expected.hex() == fields[8], "independent wire oracle mismatch"
    return dict(protocol=protocol, policy=policy, maximum=maximum, size=size,
                pattern=PATTERNS[pattern], chunk=chunk, wire_bytes=wire_size,
                wire_hex=fields[8], payload_sha256=hashlib.sha256(body).hexdigest(), samples=[])


def collect_core(port, policy):
    port.reset_input_buffer()
    port.write(b"B")
    port.flush()
    groups = []
    header = None
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        line = port.read_until(b"\n", 4096)
        if not line:
            continue
        assert line.endswith(b"\n"), f"partial benchmark line: {line[:80]!r}"
        fields = line.decode("ascii").strip().split(",")
        if fields[0] == "BEGIN":
            assert header is None and len(fields) == 7
            version, policy_id, core, cpuid, ccr, dwt = map(int, fields[1:])
            assert version == 1 and policy_id == POLICIES.index(policy) and core == 600000000
            assert ((cpuid >> 4) & 0xFFF) == 0xC27 and ccr & (3 << 16) == 3 << 16 and dwt & 1
            header = dict(version=version, core_clock=core, cpuid=cpuid, ccr=ccr, dwt_ctrl=dwt)
        elif fields[0] == "G":
            assert header is not None and (not groups or len(groups[-1]["samples"]) == 9)
            groups.append(core_group(fields, policy))
        elif fields[0] == "R":
            index, iterations, rx, tx, release, window, ok = map(int, fields[1:])
            group = groups[-1]
            assert index == len(group["samples"]) and index < 9 and ok == 1
            assert iterations == (1 if group["size"] == 1024 else 4)
            assert 0 < rx + tx + release <= window < 600000
            assert min(rx, tx, release) > 0
            group["samples"].append(dict(index=index, iterations=iterations,
                                        rx_cycles=rx, tx_cycles=tx, release_cycles=release,
                                        irq_off_cycles=window))
        elif fields[0] == "END":
            assert fields[1:] == ["75", "0"] and len(groups) == 75
            assert all(len(group["samples"]) == 9 for group in groups)
            keys = {(g["protocol"], g["size"], g["pattern"], g["chunk"]) for g in groups}
            expected = {(proto, size, pattern, chunk) for size in SIZES for pattern in PATTERNS
                        for proto, chunk in (("cobs", 0), ("cobs", 128), ("rtu", 0))}
            assert keys == expected
            return dict(header=header, groups=groups)
        else:
            raise AssertionError(f"unknown benchmark line {fields[0]}")
    raise TimeoutError("paired core benchmark did not complete")


def link_for(port, protocol, policy):
    if protocol == "rtu":
        return rtu.HardwareLink(port, policy_for(policy))
    cobs.CRC_MODE = policy
    cobs.CRC_SIZE = 0 if policy == "none" else 2
    cobs.MAX_PAYLOAD = 253
    cobs.LENGTH_SIZE = 1
    return cobs.HardwareLink(port)


def read_exact(port, count, timeout):
    deadline = time.monotonic() + timeout
    received = bytearray()
    while len(received) < count and time.monotonic() < deadline:
        received.extend(port.read(count - len(received)))
    return bytes(received)


def corpus(case):
    _name, sizes, pattern = case
    return tuple(payload(size, variant % 5 if pattern == -1 else pattern)
                 for variant in range(10) for size in sizes)


def target_fps(case, baud):
    # Same cadence for every protocol/policy; budget for the longest reference
    # request+reply pair and leave host/turnaround margin. Not RTU t3.5 timing.
    maximum_wire = max(len(wire(proto, policy, body)) for body in corpus(case)
                       for proto in ("cobs", "rtu") for policy in POLICIES)
    return min(300, max(1, math.floor(0.75 * baud / (20 * maximum_wire))))


def collect_uart(port, protocol, policy, seconds=2, repeats=2):
    link = link_for(port, protocol, policy)
    hello = link.hello()
    result = []
    prepared = [(case, corpus(case)) for case in CASES]
    for repeat in range(repeats):
        for case, bodies in prepared if repeat % 2 == 0 else reversed(prepared):
            frames = tuple(wire(protocol, policy, body) for body in bodies)
            rate = target_fps(case, port.baudrate)
            count = rate * seconds
            digest = hashlib.sha256()
            for body in bodies:
                digest.update(len(body).to_bytes(4, "little")); digest.update(body)
            link.reset_metrics()
            if protocol == "cobs": time.sleep(0.03)  # Both reset paths now settle for 80 ms.
            start = time.monotonic()
            lateness = 0.0
            wire_bytes = 0
            data_bytes = 0
            for i in range(count):
                due = start + i / rate
                remaining = due - time.monotonic()
                if remaining > 0: time.sleep(remaining)
                lateness = max(lateness, time.monotonic() - due)
                frame = frames[i % len(frames)]
                assert port.write(frame) == len(frame)
                port.flush()
                received = read_exact(port, len(frame), 2)
                assert received == frame, f"{protocol}/{policy} {case[0]} echo mismatch {received.hex()}"
                wire_bytes += len(frame)
                data_bytes += len(bodies[i % len(bodies)])
            # Equal complete schedule windows, including the final idle part.
            remaining = start + seconds - time.monotonic()
            if remaining > 0: time.sleep(remaining)
            elapsed = time.monotonic() - start
            stats = link.stats()
            if protocol == "cobs":
                cobs.healthy_failures(stats)
                cobs.assert_plain_echo_accounting(stats, count, data_bytes)
                release = "cobs_tx_release"
            else:
                rtu.healthy_failures(stats)
                rtu.assert_plain_accounting(stats, count, data_bytes)
                release = "rtu_tx_release"
            cycles = sum(stats["counters"][n]["total"] for n in
                         ("usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow", "packet_process", release))
            row = dict(protocol=protocol, policy=policy, baud=port.baudrate,
                       case=case[0], repeat=repeat, frames=count, data_bytes=data_bytes,
                       wire_bytes_per_direction=wire_bytes, target_fps=rate,
                       corpus_sha256=digest.hexdigest(), host_seconds=elapsed,
                       max_lateness_ms=lateness * 1000, stats=stats,
                       instrumented_cycles=cycles, cycles_per_echo=cycles / count,
                       cpu_percent=100 * cycles / (hello["core_clock"] * stats["window_ms"] / 1000),
                       normalized_cpu_percent=100 * cycles / count * rate / hello["core_clock"],
                       actual_wire_percent=100 * wire_bytes * 10 / (port.baudrate * stats["window_ms"] / 1000))
            result.append(row)
            print(f"PASS UART {protocol}/{policy} {port.baudrate} {case[0]} #{repeat + 1}: "
                  f"{count} frames @{rate}/s, {row['cpu_percent']:.3f}% CPU", flush=True)
    return dict(hello=hello, rows=result)


def collect_probe(port, protocol):
    link = link_for(port, protocol, "bitwise")
    result = dict(protocol=protocol, baud=port.baudrate, policy="bitwise", trials=[])
    try:
        result["hello"] = link.hello()
        link.reset_metrics()
        for size in (8, 32, 128, 252):
            frame = wire(protocol, "bitwise", payload(size, 0))
            for repeat in range(3):
                port.reset_input_buffer()
                assert port.write(frame) == len(frame)
                port.flush()
                received = read_exact(port, len(frame), 0.3)
                result["trials"].append(dict(size=size, repeat=repeat, expected=frame.hex(),
                                             received=received.hex(), exact=received == frame))
                time.sleep(0.01)
        try: result["stats"] = link.stats()
        except Exception as exc: result["stats_error"] = f"{type(exc).__name__}: {exc}"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    result["all_exact"] = len(result["trials"]) == 12 and all(t["exact"] for t in result["trials"])
    print(f"PROBE {protocol} {port.baudrate}: {sum(t['exact'] for t in result['trials'])}/12 exact", flush=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--core-only", action="store_true")
    parser.add_argument("--programmer", default=r"C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe")
    parser.add_argument("--bash", default=r"C:\Program Files\Git\bin\bash.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists(): parser.error("refusing to overwrite existing evidence")
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    session = REPO / f"wire/tests/out/comparison-{stamp}"
    session.mkdir(parents=True, exist_ok=False)
    backup = session / "before.bin"
    record = dict(schema=1, timestamp_utc=datetime.now(timezone.utc).isoformat(),
                  source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                  stlink_serial=args.serial, port=args.port, session=str(session),
                  status="running", core=[], uart=[], probes=[])
    sources = {Path(__file__).resolve(), HERE / "protocol_bench.cpp", HERE / "build.sh"}
    for folder in ("cobs", "crc", "wire", "uart", "modbus", "modbus/rtu"):
        sources.update((REPO / folder).glob("*.h")); sources.update((REPO / folder / "detail").glob("*.h"))
    sources.update((REPO / "cobs").glob("*.cpp"))
    for folder, name in (("cobs", "cobs"), ("modbus/rtu", "modbus")):
        root = REPO / folder / "tests/hardware/h7s"
        sources.update(root / n for n in ("build.sh", f"{name}_bench.cpp", f"{name}_hardware.py"))
    cube = REPO / "stm32_cube_test/h7s_cobs_test"
    sources.update((cube / "Boot/Core").rglob("*.c"))
    sources.update((cube / "Boot/Core").rglob("*.h"))
    sources.update((cube / "Boot/Core/Startup").glob("*.s"))
    sources.add(cube / "Boot/STM32H7S3L8HX_FLASH.ld")
    record["source_sha256"] = {p.relative_to(REPO).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(sources)}
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]
    mutated = False

    def command(tag, argv, env=None):
        with (session / f"{tag}.log").open("w", encoding="utf-8") as log:
            subprocess.run(argv, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    def build_flash(kind, policy, baud):
        import os
        environment = os.environ.copy()
        if kind == "core":
            script = HERE / "build.sh"
            environment["PROTOCOL_BENCH_CRC"] = str(POLICIES.index(policy))
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/protocol-comparison/protocol_bench.elf"
        elif kind == "cobs":
            script = REPO / "cobs/tests/hardware/h7s/build.sh"
            environment.update(COBS_HW_BAUD=str(baud), COBS_HW_CRC=str(POLICIES.index(policy)), COBS_HW_MAX_PAYLOAD="253")
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/cobs-hardware/cobs_hardware_bench.elf"
        else:
            script = REPO / "modbus/rtu/tests/hardware/h7s/build.sh"
            environment.update(MODBUS_HW_BAUD=str(baud), MODBUS_HW_CRC_POLICY="nocrc" if policy == "none" else policy,
                               MODBUS_HW_OPT="-Os", MODBUS_HW_LTO="0")
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf"
        tag = f"{kind}-{policy}-{baud}"
        print(f"BUILD + FLASH {tag}", flush=True)
        command(tag + "-build", [args.bash, str(script)], environment)
        image = elf.read_bytes()
        (session / f"{tag}.elf").write_bytes(image)
        nonlocal mutated
        mutated = True
        command(tag + "-flash", [args.programmer, *connection, "-w", str(elf), "-v", "-rst"])
        return hashlib.sha256(image).hexdigest()

    def save():
        output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert backup.stat().st_size == 65536
    record["backup_sha256"] = hashlib.sha256(backup.read_bytes()).hexdigest()
    try:
        for policy in POLICIES:
            image = build_flash("core", policy, 115200)
            with serial.Serial(args.port, 115200, timeout=1, write_timeout=10) as port:
                time.sleep(0.25)
                core = collect_core(port, policy)
            core.update(policy=policy, elf_sha256=image)
            record["core"].append(core); save()
            print(f"PASS CORE {policy}: 75 groups, 675 windows, exact independent wire vectors", flush=True)
        if not args.core_only:
            for baud in (115200, 1000000):
                for policy in POLICIES:
                    for protocol in ("rtu", "cobs") if policy == "bitwise" else ("cobs", "rtu"):
                        image = build_flash(protocol, policy, baud)
                        with serial.Serial(args.port, baud, timeout=0.02, write_timeout=10) as port:
                            time.sleep(0.25); port.reset_input_buffer()
                            result = collect_uart(port, protocol, policy)
                        result.update(protocol=protocol, policy=policy, baud=baud, elf_sha256=image)
                        record["uart"].append(result); save()
            for baud in (3000000, 6000000, 10000000):
                for protocol in ("cobs", "rtu"):
                    image = build_flash(protocol, "bitwise", baud)
                    with serial.Serial(args.port, baud, timeout=0.02, write_timeout=10) as port:
                        time.sleep(0.25); port.reset_input_buffer()
                        probe = collect_probe(port, protocol)
                    probe["elf_sha256"] = image
                    record["probes"].append(probe); save()
        record["status"] = "passed"
    except BaseException as exc:
        record.update(status="failed", error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        try:
            if mutated:
                print("Restoring and verifying original firmware...", flush=True)
                command("restore", [args.programmer, *connection, "-w", str(backup), "0x08000000", "-v", "-rst"])
                record["restored_and_verified"] = True
        finally:
            record["finished_utc"] = datetime.now(timezone.utc).isoformat()
            save()


if __name__ == "__main__":
    main()
