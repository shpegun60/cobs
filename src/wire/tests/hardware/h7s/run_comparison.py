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

def repository_root(start: Path) -> Path:
    """The git root: the directory holding COBS.pro, found by walking up from `start`."""
    for candidate in (start, *start.parents):
        if (candidate / "COBS.pro").is_file():
            return candidate
    raise RuntimeError(f"repository root (COBS.pro) not found above {start}")


REPO = repository_root(Path(__file__).resolve().parent)   # the git root
SRC = REPO / "src"
HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(SRC / "cobs/tests/hardware/h7s"), str(SRC / "modbus/rtu/tests/hardware/h7s")]
import cobs_hardware as cobs
import modbus_hardware as rtu
import serial

POLICIES = ("none", "bitwise", "table")
# "rtu-framed" is the RTU harness built with MODBUS_HW_FRAMER=1: the same
# Uart<256,4> and Pool<8,2>, the endpoint carrying the framing policy, every
# function length-prefixed ([N: BE16][body]) so its useful body is two bytes
# shorter (250 in a 256-byte ADU with CRC16).
PROTOCOLS = ("cobs", "rtu", "rtu-framed")
FRAMED_PREFIX = 2
PATTERNS = ("random", "zero", "nonzero", "alternating", "boundary")
SIZES = (8, 32, 128, 252, 1024)
# The seven original scenarios are the default and the "full run"; random250
# is the largest body all three links can carry and exists for the
# three-way comparison.
DEFAULT_CASES = (("random8", (8,), 0), ("random32", (32,), 0),
                 ("random128", (128,), 0), ("random252", (252,), 0),
                 ("zero252", (252,), 1), ("nonzero252", (252,), 2),
                 ("mixed", (8, 32, 128, 252), -1))
CASES = DEFAULT_CASES + (("random250", (250,), 0),)
DEFAULT_BAUDS = (115200, 1000000)
PROBE_BAUDS = (3000000, 6000000, 10000000)
# The COBS harness's Uart<ChunkSize, ChunkCount>; the RTU harness is fixed at 256x4.
DEFAULT_COBS_UART = (128, 8)


def parse_cobs_uart(text):
    """'128x8,256x4' -> ((128, 8), (256, 4)), each a COBS-harness Uart<size, count>."""
    configs = []
    for item in text.split(","):
        size, count = (int(part) for part in item.strip().lower().split("x"))
        assert size >= 64 and count >= 1 and size * count <= 4096, f"unsupported UART geometry {item}"
        configs.append((size, count))
    assert len(set(configs)) == len(configs), "duplicate UART geometry"
    return tuple(configs)


def image_tag(kind, policy, baud, cobs_uart=None):
    """Session file stem of one flashed image; the chunk suffix exists only when it was selected."""
    tag = f"{kind}-{policy}-{baud}"
    return f"{tag}-{cobs_uart[0]}x{cobs_uart[1]}" if cobs_uart else tag


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
    if protocol == "rtu-framed":
        assert len(body) <= 256 - 2 - FRAMED_PREFIX - policy_for(policy).wire_size, "body exceeds the framed ADU"
        return policy_for(policy).append(b"\x11\x41" + len(body).to_bytes(FRAMED_PREFIX, "big") + body)
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


def link_for(port, protocol, policy, cobs_uart=DEFAULT_COBS_UART):
    if protocol in ("rtu", "rtu-framed"):
        # The peer module's framing mode is process state; set it for THIS link.
        rtu.FRAMED = protocol == "rtu-framed"
        rtu.FRAME_PREFIX = FRAMED_PREFIX if rtu.FRAMED else 0
        return rtu.HardwareLink(port, policy_for(policy))
    cobs.CRC_MODE = policy
    cobs.CRC_SIZE = 0 if policy == "none" else 2
    cobs.MAX_PAYLOAD = 253
    cobs.LENGTH_SIZE = 1
    # HELLO must report exactly the geometry that was built in.
    cobs.UART_CHUNK_SIZE, cobs.UART_CHUNK_COUNT = cobs_uart
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


def collect_uart(port, protocol, policy, seconds=2, repeats=2, cases=CASES, cobs_uart=DEFAULT_COBS_UART):
    link = link_for(port, protocol, policy, cobs_uart)
    hello = link.hello()
    result = []
    prepared = [(case, corpus(case)) for case in cases]
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
        for size in (8, 32, 128, 250 if protocol == "rtu-framed" else 252):
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
    parser.add_argument("--core-only", action="store_true", help="endpoint-only cycles, no UART traffic or probes")
    parser.add_argument("--uart-only", action="store_true", help="UART traffic only, no endpoint-only cycles or probes")
    parser.add_argument("--protocols", default="cobs,rtu", help="subset of cobs,rtu,rtu-framed for the UART traffic and probes")
    parser.add_argument("--policies", default=",".join(POLICIES), help="subset of none,bitwise,table")
    parser.add_argument("--bauds", default=",".join(map(str, DEFAULT_BAUDS)), help="UART traffic bauds")
    parser.add_argument("--cases", default=",".join(c[0] for c in DEFAULT_CASES), help="UART traffic scenarios")
    parser.add_argument("--cobs-uart", default="128x8",
                        help="COBS harness Uart<ChunkSize,ChunkCount> geometries, e.g. 128x8,256x4; each is a separate build")
    parser.add_argument("--programmer", default=r"C:\ST\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe")
    parser.add_argument("--bash", default=r"C:\Program Files\Git\bin\bash.exe")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists(): parser.error("refusing to overwrite existing evidence")
    if args.core_only and args.uart_only: parser.error("--core-only and --uart-only exclude each other")
    protocols = tuple(p for p in PROTOCOLS if p in set(args.protocols.split(",")))
    policies = tuple(p for p in POLICIES if p in set(args.policies.split(",")))
    bauds = tuple(int(b) for b in args.bauds.split(","))
    cases = tuple(c for c in CASES if c[0] in set(args.cases.split(",")))
    cobs_uart = parse_cobs_uart(args.cobs_uart)
    if not (protocols and policies and bauds and cases): parser.error("an empty selection measures nothing")
    full = (not args.core_only and not args.uart_only and protocols == ("cobs", "rtu") and policies == POLICIES
            and bauds == DEFAULT_BAUDS and cases == DEFAULT_CASES and cobs_uart == (DEFAULT_COBS_UART,))
    # A narrowed run records exactly what it measured, so the verifier expects
    # that and nothing more; a full run records no selection, like the
    # original evidence files.
    selection = None if full else dict(
        core_only=args.core_only, uart_only=args.uart_only, protocols=list(protocols),
        policies=list(policies), bauds=list(bauds), cases=[c[0] for c in cases],
        cobs_uart=[list(c) for c in cobs_uart])
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    session = SRC / f"wire/tests/out/comparison-{stamp}"
    session.mkdir(parents=True, exist_ok=False)
    backup = session / "before.bin"
    record = dict(schema=1, timestamp_utc=datetime.now(timezone.utc).isoformat(),
                  source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                  stlink_serial=args.serial, port=args.port, session=str(session),
                  selection=selection, status="running", core=[], uart=[], probes=[])
    sources = {Path(__file__).resolve(), HERE / "protocol_bench.cpp", HERE / "build.sh"}
    for folder in ("cobs", "crc", "wire", "uart", "modbus", "modbus/rtu"):
        sources.update((SRC / folder).glob("*.h")); sources.update((SRC / folder / "detail").glob("*.h"))
    sources.update((SRC / "cobs").glob("*.cpp"))
    for folder, name in (("cobs", "cobs"), ("modbus/rtu", "modbus")):
        root = SRC / folder / "tests/hardware/h7s"
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

    def build_flash(kind, policy, baud, uart=None):
        import os
        environment = os.environ.copy()
        if kind == "core":
            script = HERE / "build.sh"
            environment["PROTOCOL_BENCH_CRC"] = str(POLICIES.index(policy))
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/protocol-comparison/protocol_bench.elf"
        elif kind == "cobs":
            script = SRC / "cobs/tests/hardware/h7s/build.sh"
            environment.update(COBS_HW_BAUD=str(baud), COBS_HW_CRC=str(POLICIES.index(policy)), COBS_HW_MAX_PAYLOAD="253")
            if uart is not None:
                environment.update(COBS_HW_UART_CHUNK_SIZE=str(uart[0]), COBS_HW_UART_CHUNK_COUNT=str(uart[1]))
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/cobs-hardware/cobs_hardware_bench.elf"
        else:
            script = SRC / "modbus/rtu/tests/hardware/h7s/build.sh"
            environment.update(MODBUS_HW_BAUD=str(baud), MODBUS_HW_CRC_POLICY="nocrc" if policy == "none" else policy,
                               MODBUS_HW_OPT="-Os", MODBUS_HW_LTO="0",
                               MODBUS_HW_FRAMER="1" if kind == "rtu-framed" else "0")
            elf = REPO / "stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf"
        tag = image_tag(kind, policy, baud, uart)
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
        if not args.uart_only:
            for policy in policies:
                image = build_flash("core", policy, 115200)
                with serial.Serial(args.port, 115200, timeout=1, write_timeout=10) as port:
                    time.sleep(0.25)
                    core = collect_core(port, policy)
                core.update(policy=policy, elf_sha256=image)
                record["core"].append(core); save()
                print(f"PASS CORE {policy}: 75 groups, 675 windows, exact independent wire vectors", flush=True)
        if not args.core_only:
            for baud in bauds:
                for policy in policies:
                    for protocol in ("rtu", "rtu-framed", "cobs") if policy == "bitwise" else ("cobs", "rtu", "rtu-framed"):
                        if protocol not in protocols: continue
                        # One image per COBS UART geometry; the RTU harness has one.
                        for uart in (cobs_uart if protocol == "cobs" else (None,)):
                            image = build_flash(protocol, policy, baud, uart)
                            with serial.Serial(args.port, baud, timeout=0.02, write_timeout=10) as port:
                                time.sleep(0.25); port.reset_input_buffer()
                                result = collect_uart(port, protocol, policy, cases=cases,
                                                      cobs_uart=uart or DEFAULT_COBS_UART)
                            result.update(protocol=protocol, policy=policy, baud=baud, elf_sha256=image,
                                          uart_chunk=list(uart) if uart else None)
                            record["uart"].append(result); save()
        if not args.core_only and not args.uart_only:
            for baud in PROBE_BAUDS:
                for protocol in protocols:
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
