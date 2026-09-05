#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Matched-corpus live COBS/UART measurements; reuses the unchanged v2 firmware."""

from __future__ import annotations

import argparse
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import time

import cobs_hardware as peer


BAUDS = (115200, 1000000, 3000000, 6000000, 10000000)
POLICIES = ("none", "bitwise", "table")
IRQ_COUNTERS = ("usart_irq", "rx_dma_irq", "tx_dma_irq")
THREAD_COUNTERS = ("uart_slow", "packet_process", "cobs_tx_release")
REPO = Path(__file__).resolve().parents[4]


@dataclass(frozen=True)
class Case:
    name: str
    sizes: tuple[int, ...]
    pattern: str
    window: int = 7


def cases(maximum: int) -> tuple[Case, ...]:
    if maximum == 253:
        return (
            Case("latency8", (8,), "random", 1),
            Case("short32", (32,), "random"),
            Case("random253", (253,), "random"),
            Case("zero253", (253,), "zero"),
            Case("nonzero253", (253,), "nonzero"),
            Case("mixed253", (8, 32, 64, 127, 128, 253), "mixed"),
        )
    if maximum == 1024:
        return (
            Case("random1024", (1024,), "random"),
            Case("zero1024", (1024,), "zero"),
            Case("nonzero1024", (1024,), "nonzero"),
            Case("mixed1024", (32, 127, 128, 253, 254, 255, 256, 511, 512, 1024), "mixed"),
        )
    raise ValueError("comparison geometries are explicitly 253/H1 and 1024/H2")


def configure(policy: str, maximum: int) -> None:
    if policy not in POLICIES or maximum not in (253, 1024):
        raise ValueError("unsupported comparison configuration")
    peer.CRC_MODE = policy
    peer.CRC_SIZE = 0 if policy == "none" else 2
    peer.MAX_PAYLOAD = maximum
    peer.LENGTH_SIZE = 1 if maximum + peer.CRC_SIZE <= 255 else 2


def corpus(case: Case) -> tuple[tuple[bytes, ...], tuple[bytes, ...], str]:
    # Every size gets every pattern in a mixed corpus, not correlated pairs.
    patterns = ("random", "zero", "nonzero", "alternating", "boundary")
    bodies = tuple(
        peer.pattern(patterns[variant % len(patterns)] if case.pattern == "mixed"
                     else case.pattern, size, 0xC0B50000 ^ (variant << 16) ^ size)
        for variant in range(10) for size in case.sizes
    )
    encoded = tuple(peer.engine_frame(body) for body in bodies)
    for body, frame in zip(bodies, encoded, strict=True):
        if peer.decode_engine(frame[:-1]) != body:
            raise AssertionError("precomputed corpus oracle mismatch")
    digest = hashlib.sha256()
    for body in bodies:
        digest.update(len(body).to_bytes(4, "little"))
        digest.update(body)
    return bodies, encoded, digest.hexdigest()


class WireReader:
    """Compare encoded replies directly: no host CRC/COBS work while timed."""

    def __init__(self, port):
        self.port = port
        self.partial = bytearray()
        self.frames: deque[bytes] = deque()

    def read(self, timeout: float = 8.0) -> bytes:
        deadline = time.monotonic() + timeout
        while not self.frames:
            if time.monotonic() >= deadline:
                raise TimeoutError(f"echo timeout; {len(self.partial)} partial wire bytes")
            self.partial.extend(self.port.read(min(max(1, self.port.in_waiting), 65536)))
            parts = self.partial.split(b"\0")
            if len(parts) > 1:
                self.frames.extend(bytes(part) + b"\0" for part in parts[:-1])
                self.partial = bytearray(parts[-1])
        return self.frames.popleft()


def metrics(stats: dict, core: int, baud: int, frames: int,
            payload_bytes: int, wire_bytes: int) -> dict:
    seconds = stats["window_ms"] / 1000.0
    if seconds <= 0 or core <= 0 or frames <= 0 or wire_bytes <= 0:
        raise ValueError("non-positive measurement denominator")
    counters = stats["counters"]
    irq = sum(counters[name]["total"] for name in IRQ_COUNTERS)
    thread = sum(counters[name]["total"] for name in THREAD_COUNTERS)
    total = irq + thread
    # onRx/consume is INSIDE proceedSlow; send is INSIDE packet_process.
    # Never sum those nested diagnostic scopes for integrated cost.
    scale = 100.0 / (core * seconds)
    return {
        "instrumented_cycles": total,
        "irq_cycles": irq,
        "thread_inclusive_cycles": thread,
        "instrumented_cpu_percent": total * scale,
        # These are bounds for the UNION OF INSTRUMENTED SCOPES only, not
        # bounds for the whole program: IRQ preemption may be counted twice.
        "scope_union_lower_percent": max(irq, thread) * scale,
        "scope_union_upper_percent": total * scale,
        "irq_cpu_percent": irq * scale,
        "rx_consume_cpu_percent": counters["cobs_consume"]["total"] * scale,
        "packet_process_cpu_percent": counters["packet_process"]["total"] * scale,
        "cycles_per_echo": total / frames,
        "cycles_per_payload_byte": total / payload_bytes if payload_bytes else None,
        "cycles_per_wire_byte": total / wire_bytes,
        "payload_bytes_per_second": payload_bytes / seconds,
        "wire_bytes_per_second_per_direction": wire_bytes / seconds,
        "wire_utilization_percent_per_direction": 100.0 * wire_bytes * 10 / (baud * seconds),
        # Equal RX + echoed TX at full-duplex 8N1. A LINEAR EXTRAPOLATION,
        # not proof of sustainable throughput or of CPU load at a full line.
        "extrapolated_full_line_cpu_percent": 100.0 * total / wire_bytes * (baud / 10) / core,
    }


def measure(link: peer.HardwareLink, hello: dict, case: Case, seconds: float,
            prepared: tuple, result: dict) -> None:
    bodies, encoded, digest = prepared
    # Warm both code/data paths before each independent metric-reset window.
    link.echo(bodies[0])
    link.reset_metrics()
    if link.rx or link.frames:
        raise AssertionError("pending control bytes before measurement")
    reader = WireReader(link.port)
    pending: deque[int] = deque()
    counts = [0] * len(bodies)
    frames = 0
    result.update(corpus_sha256=digest, corpus_frames=len(bodies), window=case.window,
                  sizes=list(case.sizes), pattern=case.pattern,
                  requested_seconds=seconds)
    start = time.monotonic()
    deadline = start + seconds
    try:
        while time.monotonic() < deadline or pending:
            batch = []
            while len(pending) < case.window and time.monotonic() < deadline:
                index = frames % len(bodies)
                pending.append(index)
                batch.append(encoded[index])
                counts[index] += 1
                frames += 1
            if batch:
                data = b"".join(batch)
                written = link.port.write(data)
                if written != len(data):
                    raise IOError(f"short host write: {written}/{len(data)}")
            if not pending:
                break
            expected = encoded[pending.popleft()]
            received = reader.read()
            if received != expected:
                raise AssertionError(
                    f"wire echo mismatch: received {len(received)}, expected {len(expected)} bytes")
        if reader.partial or reader.frames:
            raise AssertionError("extra or partial echo after all expected frames")
    finally:
        result.update(host_seconds=time.monotonic() - start, frames=frames,
                      corpus_counts=counts,
                      payload_bytes=sum(n * len(b) for n, b in zip(counts, bodies, strict=True)),
                      wire_bytes_per_direction=sum(n * len(b) for n, b in zip(counts, encoded, strict=True)))
    # No timed traffic remains. The two-page counter snapshot is coherent;
    # its first RX control packet is included, its TX response is not.
    stats = link.stats()
    result["stats"] = stats
    peer.healthy_failures(stats)
    peer.assert_plain_echo_accounting(stats, frames, result["payload_bytes"])
    result.update(metrics(stats, hello["core_clock"], hello["baud"], frames,
                          result["payload_bytes"], result["wire_bytes_per_direction"]))


def evidence() -> dict:
    here = Path(__file__).resolve().parent
    sources = [here / n for n in ("cobs_performance.py", "run_performance.ps1",
                                  "cobs_hardware.py", "cobs_bench.cpp", "build.sh")]
    for directory in ("cobs", "crc", "wire", "uart"):
        sources.extend((REPO / directory).glob("*.h"))
        sources.extend((REPO / directory / "detail").glob("*.h"))
    sources.extend((REPO / "cobs").glob("*.cpp"))
    cube = REPO / "stm32_cube_test/h7s_cobs_test"
    sources.extend((cube / "Boot/Core").rglob("*.c"))
    sources.extend((cube / "Boot/Core").rglob("*.h"))
    sources.extend((cube / "Boot/Core/Startup").glob("*.s"))
    sources.append(cube / "Boot/STM32H7S3L8HX_FLASH.ld")
    elf = cube / "out/cobs-hardware/cobs_hardware_bench.elf"
    return {
        "source_base_commit": subprocess.check_output(
            ["git", "-C", str(REPO), "rev-parse", "HEAD"], text=True).strip(),
        "source_sha256": {p.relative_to(REPO).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted(set(sources))},
        "elf_sha256": hashlib.sha256(elf.read_bytes()).hexdigest(),
        "host": platform.platform(), "python": platform.python_version(),
        "compiler": "GNU Arm 14.3.1", "optimization": "-Os, no LTO",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, choices=BAUDS, required=True)
    parser.add_argument("--crc", choices=POLICIES, required=True)
    parser.add_argument("--max-payload", type=int, choices=(253, 1024), required=True)
    parser.add_argument("--seconds", type=float, default=2.0)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--case", action="append")
    parser.add_argument("--stlink-serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0 or args.repeats < 1:
        parser.error("seconds and repeats must be positive")
    configure(args.crc, args.max_payload)
    peer.reference_self_check()
    selected = tuple(c for c in cases(args.max_payload) if not args.case or c.name in args.case)
    if not selected or (args.case and set(args.case) != {c.name for c in selected}):
        parser.error("unknown case for selected geometry")
    prepared = {c.name: corpus(c) for c in selected}
    identity = evidence()
    with peer.serial.Serial(args.port, args.baud, timeout=0.02, write_timeout=10) as port:
        time.sleep(0.25)
        port.reset_input_buffer()
        port.reset_output_buffer()
        link = peer.HardwareLink(port)
        hello = link.hello()
        for repeat in range(args.repeats):
            # Reverse case order on the second repeat to reduce order bias.
            for case in selected if repeat % 2 == 0 else reversed(selected):
                record = {
                    "schema": 1, "suite": "matched_performance", "status": "running",
                    "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                    "crc": args.crc, "max_payload": args.max_payload, "baud": args.baud,
                    "case": case.name, "repeat": repeat, "hello": hello,
                    "port": args.port, "stlink_serial": args.stlink_serial, **identity,
                }
                try:
                    measure(link, hello, case, args.seconds, prepared[case.name], record)
                    record["status"] = "passed"
                except Exception as exc:
                    record.update(status="failed", error=f"{type(exc).__name__}: {exc}")
                    raise
                finally:
                    with args.output.open("a", encoding="utf-8") as output:
                        output.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
                print(f"PASS {args.crc}/{args.max_payload} {args.baud} {case.name} #{repeat + 1}: "
                      f"{record['frames']} echoes, {record['instrumented_cpu_percent']:.3f}% CPU, "
                      f"{record['wire_utilization_percent_per_direction']:.1f}% wire, "
                      f"{record['cycles_per_echo']:.0f} cycles/echo", flush=True)


if __name__ == "__main__":
    main()
