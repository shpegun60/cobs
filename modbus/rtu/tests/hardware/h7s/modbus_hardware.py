#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Independent PC oracle and real-silicon Modbus RTU/UART test runner."""

from __future__ import annotations

import argparse
from collections.abc import Iterable
from dataclasses import dataclass
import json
from pathlib import Path
import random
import struct
import time

import serial


MAX_DATA_SIZE = 252
MAX_ADU_SIZE = 256
UART_CHUNK_SIZE = 256
UART_CHUNK_COUNT = 4
RX_BLOCKS = 8
TX_BLOCKS = 2

CONTROL_ADDRESS = 0xF7
CONTROL_FUNCTION = 0x41
MAGIC = b"MRTU"
PROTOCOL_VERSION = 2
CRC_POLICIES = {"bitwise": 0, "table": 1}

CMD_HELLO = 1
CMD_STATS = 2
CMD_RESET = 3
CMD_HOLD = 4
CMD_SELFTEST = 5

STATS_FIELDS = (
    "version",
    "window_ms",
    "echo_frames",
    "echo_data_bytes",
    "control_frames",
    "response_failures",
    "selftest_failures",
    "uart_rx_overrun",
    "uart_rx_errors",
    "uart_tx_errors",
    "uart_restarts",
    "rtu_candidates",
    "rtu_frames_received",
    "rtu_crc_errors",
    "rtu_too_short",
    "rtu_oversize",
    "rtu_allocation_failure",
    "rtu_stream_gaps",
    "rtu_frames_sent",
    "rtu_send_refused_busy",
    "rtu_send_failed",
    "rx_available",
    "rx_in_use",
    "rx_high_water",
    "rx_exhausted",
    "rx_rejected",
    "tx_available",
    "tx_in_use",
    "tx_high_water",
    "tx_exhausted",
    "tx_rejected",
)

COUNTER_NAMES = (
    "usart_irq",
    "rx_dma_irq",
    "tx_dma_irq",
    "uart_slow",
    "rtu_receive",
    "packet_process",
    "rtu_tx_release",
)


def make_crc_table() -> tuple[int, ...]:
    table = []
    for byte in range(256):
        value = byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
        table.append(value)
    return tuple(table)


CRC_TABLE = make_crc_table()


def crc16(data: bytes) -> int:
    value = 0xFFFF
    for byte in data:
        value = (value >> 8) ^ CRC_TABLE[(value ^ byte) & 0xFF]
    return value


def make_adu(address: int, function: int, data: bytes = b"") -> bytes:
    if not 0 <= address <= 0xFF:
        raise ValueError("address must fit one byte")
    if not 0 <= function <= 0xFF:
        raise ValueError("function must fit one byte")
    if len(data) > MAX_DATA_SIZE:
        raise ValueError("function data exceeds the Modbus PDU limit")
    body = bytes((address, function)) + data
    checksum = crc16(body)
    return body + bytes((checksum & 0xFF, checksum >> 8))


@dataclass(frozen=True)
class Adu:
    address: int
    function: int
    data: bytes
    wire: bytes


def parse_adu(wire: bytes) -> Adu:
    if not 4 <= len(wire) <= MAX_ADU_SIZE:
        raise AssertionError(f"invalid ADU size from board: {len(wire)}")
    expected = crc16(wire[:-2])
    received = wire[-2] | (wire[-1] << 8)
    if received != expected:
        raise AssertionError(
            f"board response CRC mismatch: {received:04X} != {expected:04X}"
        )
    return Adu(wire[0], wire[1], wire[2:-2], wire)


def reference_self_check() -> None:
    known = bytes((0x01, 0x03, 0x00, 0x00, 0x00, 0x0A))
    if crc16(known) != 0xCDC5:
        raise AssertionError("PC CRC oracle failed the canonical C5 CD vector")
    known_adu = known + b"\xC5\xCD"
    if parse_adu(known_adu).wire != known_adu:
        raise AssertionError("PC ADU parser rejected the canonical vector")
    for byte_index in range(len(known_adu)):
        for bit in range(8):
            damaged = bytearray(known_adu)
            damaged[byte_index] ^= 1 << bit
            try:
                parse_adu(bytes(damaged))
            except AssertionError:
                continue
            raise AssertionError("PC CRC oracle accepted a single-bit mutation")
    for size in range(MAX_DATA_SIZE + 1):
        data = bytes((index * 37 + size) & 0xFF for index in range(size))
        wire = make_adu(0x11, 0xA7, data)
        parsed = parse_adu(wire)
        if parsed.data != data or len(wire) != size + 4:
            raise AssertionError(f"PC ADU round trip failed at data size {size}")


class HardwareLink:
    def __init__(self, port: serial.Serial, expected_crc_policy: int):
        self.port = port
        self.token = 0
        self.expected_crc_policy = expected_crc_policy

    def write_candidate(self, wire: bytes, gap_after: float = 0.0) -> None:
        written = self.port.write(wire)
        if written != len(wire):
            raise AssertionError(f"short serial write: {written}/{len(wire)}")
        self.port.flush()
        if gap_after > 0:
            time.sleep(gap_after)

    def read_exact(self, size: int, timeout: float = 5.0) -> bytes:
        deadline = time.monotonic() + timeout
        received = bytearray()
        while len(received) < size and time.monotonic() < deadline:
            chunk = self.port.read(size - len(received))
            if chunk:
                received.extend(chunk)
        if len(received) != size:
            raise AssertionError(
                f"response timeout: received {len(received)}/{size} bytes"
            )
        return bytes(received)

    def assert_silence(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        unexpected = bytearray()
        while time.monotonic() < deadline:
            chunk = self.port.read(256)
            if chunk:
                unexpected.extend(chunk)
        if unexpected:
            raise AssertionError(
                f"unexpected response to rejected ADU: {unexpected.hex()}"
            )

    def echo(self, address: int, function: int, data: bytes,
             timeout: float = 5.0) -> Adu:
        request = make_adu(address, function, data)
        self.write_candidate(request)
        response = parse_adu(self.read_exact(len(request), timeout))
        if response.wire != request:
            raise AssertionError(
                "echo mismatch: "
                f"sent={request.hex()} received={response.wire.hex()}"
            )
        return response

    def control(self, command: int, payload_size: int,
                argument: int | None = None,
                timeout: float = 5.0) -> bytes:
        self.token = (self.token + 1) & 0xFFFFFFFF
        data = MAGIC + bytes((command,)) + struct.pack("<I", self.token)
        if argument is not None:
            data += struct.pack("<I", argument)
        request = make_adu(CONTROL_ADDRESS, CONTROL_FUNCTION, data)
        self.write_candidate(request)
        expected_data_size = 9 + payload_size
        response = parse_adu(self.read_exact(expected_data_size + 4, timeout))
        if response.address != CONTROL_ADDRESS or \
                response.function != CONTROL_FUNCTION:
            raise AssertionError("control response metadata mismatch")
        if len(response.data) != expected_data_size or \
                response.data[:4] != MAGIC or \
                response.data[4] != (command | 0x80) or \
                struct.unpack_from("<I", response.data, 5)[0] != self.token:
            raise AssertionError(
                f"malformed control response for command {command}: "
                f"{response.data.hex()}"
            )
        return response.data[9:]

    def hello(self) -> dict[str, int]:
        payload = self.control(CMD_HELLO, 44)
        names = (
            "version",
            "baud",
            "core_clock",
            "max_receive",
            "max_send",
            "max_frame",
            "uart_chunk_size",
            "uart_chunk_count",
            "rx_blocks",
            "tx_blocks",
            "crc_policy",
        )
        hello = dict(zip(names, struct.unpack("<11I", payload), strict=True))
        expected = {
            "version": PROTOCOL_VERSION,
            "baud": self.port.baudrate,
            "core_clock": 600_000_000,
            "max_receive": MAX_DATA_SIZE,
            "max_send": MAX_DATA_SIZE,
            "max_frame": MAX_ADU_SIZE,
            "uart_chunk_size": UART_CHUNK_SIZE,
            "uart_chunk_count": UART_CHUNK_COUNT,
            "rx_blocks": RX_BLOCKS,
            "tx_blocks": TX_BLOCKS,
            "crc_policy": self.expected_crc_policy,
        }
        if hello != expected:
            raise AssertionError(f"unexpected board geometry: {hello}")
        return hello

    def ack(self, command: int, argument: int | None = None,
            timeout: float = 5.0) -> tuple[int, int]:
        payload = self.control(command, 8, argument, timeout)
        return struct.unpack("<2I", payload)

    def reset_metrics(self) -> None:
        status, _ = self.ack(CMD_RESET)
        if status != 0:
            raise AssertionError(f"metric reset refused: {status}")
        # The baseline is taken after the ACK's DMA borrow is released.
        time.sleep(0.08)

    def stats(self) -> dict:
        scalar_bytes = 4 * len(STATS_FIELDS)
        counters_bytes = 16 * len(COUNTER_NAMES)
        payload = self.control(CMD_STATS, scalar_bytes + counters_bytes)
        values = struct.unpack_from(f"<{len(STATS_FIELDS)}I", payload)
        result: dict = dict(zip(STATS_FIELDS, values, strict=True))
        offset = scalar_bytes
        counters = {}
        for name in COUNTER_NAMES:
            total, calls, maximum = struct.unpack_from("<QII", payload, offset)
            offset += 16
            counters[name] = {
                "total": total,
                "calls": calls,
                "max": maximum,
                "avg": total // calls if calls else 0,
            }
        result["counters"] = counters
        return result


def pattern(name: str, size: int, seed: int) -> bytes:
    if name == "zero":
        return bytes(size)
    if name == "alternating":
        return bytes(0x00 if index % 2 == 0 else 0xA5
                     for index in range(size))
    if name == "random":
        rng = random.Random(seed)
        return bytes(rng.randrange(256) for _ in range(size))
    raise ValueError(name)


def assert_fields(stats: dict, **expected: int) -> None:
    wrong = {
        name: (stats[name], value)
        for name, value in expected.items()
        if stats[name] != value
    }
    if wrong:
        raise AssertionError(f"counter/accounting mismatch: {wrong}")


def assert_zero(stats: dict, names: Iterable[str]) -> None:
    bad = {name: stats[name] for name in names if stats[name] != 0}
    if bad:
        raise AssertionError(f"non-zero failure counters: {bad}")


def assert_observation_occupancy(stats: dict) -> None:
    # The STATS request itself owns one RX Packet. The snapshot is taken before
    # allocating its response Message, so no TX block may still be owned.
    assert_fields(
        stats,
        version=PROTOCOL_VERSION,
        rx_available=RX_BLOCKS - 1,
        rx_in_use=1,
        tx_available=TX_BLOCKS,
        tx_in_use=0,
    )


def healthy_failures(stats: dict) -> None:
    assert_observation_occupancy(stats)
    assert_zero(
        stats,
        (
            "response_failures",
            "selftest_failures",
            "uart_rx_overrun",
            "uart_rx_errors",
            "uart_tx_errors",
            "uart_restarts",
            "rtu_crc_errors",
            "rtu_too_short",
            "rtu_oversize",
            "rtu_allocation_failure",
            "rtu_stream_gaps",
            "rtu_send_refused_busy",
            "rtu_send_failed",
            "rx_exhausted",
            "rx_rejected",
            "tx_exhausted",
            "tx_rejected",
        ),
    )


def assert_plain_accounting(stats: dict, frames: int,
                            data_bytes: int) -> None:
    assert_fields(
        stats,
        echo_frames=frames,
        echo_data_bytes=data_bytes,
        control_frames=1,
        rtu_candidates=frames + 1,
        rtu_frames_received=frames + 1,
        rtu_frames_sent=frames,
    )
    expected_calls = {
        "rtu_receive": frames + 1,
        "packet_process": frames,
        "rtu_tx_release": frames,
        "tx_dma_irq": frames,
    }
    wrong = {
        name: (stats["counters"][name]["calls"], expected)
        for name, expected in expected_calls.items()
        if stats["counters"][name]["calls"] != expected
    }
    if wrong:
        raise AssertionError(f"DWT call accounting mismatch: {wrong}")


def suite_vectors(link: HardwareLink) -> dict:
    link.reset_metrics()
    sizes = (0, 1, 2, 31, 32, 63, 64, 127, 128, 251, 252)
    names = ("zero", "alternating", "random")
    addresses = (1, 0x11, 0xF7)
    functions = (0x03, 0x10, 0x43, 0x64, 0xA7)
    frames = 0
    data_bytes = 0
    started = time.monotonic()
    for size in sizes:
        selected = ("zero",) if size == 0 else names
        for index, name in enumerate(selected):
            data = pattern(name, size, 0x4D4F4400 ^ size ^ index)
            address = addresses[frames % len(addresses)]
            function = functions[frames % len(functions)]
            try:
                link.echo(address, function, data, timeout=8.0)
            except Exception as error:
                raise AssertionError(
                    f"vector {frames} failed: data_size={size} "
                    f"pattern={name} address={address:#04x} "
                    f"function={function:#04x}: {error}"
                ) from error
            frames += 1
            data_bytes += size
    stats = link.stats()
    healthy_failures(stats)
    assert_plain_accounting(stats, frames, data_bytes)
    return {
        "suite": "vectors",
        "frames": frames,
        "data_bytes": data_bytes,
        "seconds": time.monotonic() - started,
        "stats": stats,
    }


def suite_faults(link: HardwareLink) -> dict:
    link.reset_metrics()
    base = make_adu(1, 0x03, b"\x00\x10\x00\x02")
    mutation_indices = (0, 1, 2, len(base) - 1)
    for sequence, byte_index in enumerate(mutation_indices):
        damaged = bytearray(base)
        damaged[byte_index] ^= 0x01
        link.write_candidate(bytes(damaged))
        link.assert_silence(0.06)
        link.echo(0x11, 0x43, b"recovered" + bytes((sequence,)))
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats,
        (
            "response_failures",
            "selftest_failures",
            "uart_rx_overrun",
            "uart_rx_errors",
            "uart_tx_errors",
            "uart_restarts",
            "rtu_too_short",
            "rtu_oversize",
            "rtu_allocation_failure",
            "rtu_stream_gaps",
            "rtu_send_refused_busy",
            "rtu_send_failed",
            "rx_exhausted",
            "rx_rejected",
            "tx_exhausted",
            "tx_rejected",
        ),
    )
    good = len(mutation_indices)
    assert_fields(
        stats,
        echo_frames=good,
        echo_data_bytes=good * len(b"recovered\x00"),
        control_frames=1,
        rtu_candidates=(2 * good) + 1,
        rtu_frames_received=good + 1,
        rtu_crc_errors=good,
        rtu_frames_sent=good,
    )
    return {"suite": "faults", "corruptions": good, "stats": stats}


def suite_selftest(link: HardwareLink) -> dict:
    link.reset_metrics()
    status, _ = link.ack(CMD_SELFTEST)
    if status != 0:
        raise AssertionError(f"backpressure self-test refused: {status}")
    time.sleep(0.08)
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats,
        (
            "response_failures",
            "selftest_failures",
            "uart_rx_overrun",
            "uart_rx_errors",
            "uart_tx_errors",
            "uart_restarts",
            "rtu_crc_errors",
            "rtu_too_short",
            "rtu_oversize",
            "rtu_allocation_failure",
            "rtu_stream_gaps",
            "rtu_send_failed",
            "rx_exhausted",
            "rx_rejected",
            "tx_rejected",
        ),
    )
    assert_fields(
        stats,
        echo_frames=0,
        control_frames=2,
        rtu_candidates=2,
        rtu_frames_received=2,
        rtu_frames_sent=1,
        rtu_send_refused_busy=1,
        tx_exhausted=1,
    )
    return {"suite": "selftest", "stats": stats}


def pool_data(index: int) -> bytes:
    return b"POOL" + struct.pack("<I", index)


def suite_pool(link: HardwareLink) -> dict:
    link.reset_metrics()
    hold_ms = 500
    status, value = link.ack(CMD_HOLD, hold_ms)
    if status != 0 or value != hold_ms:
        raise AssertionError(f"packet hold refused: {status}/{value}")
    time.sleep(0.08)

    requests = [make_adu(0x11, 0x43, pool_data(index)) for index in range(16)]
    for wire in requests:
        # This explicit silence is part of the v1 physical-burst contract.
        link.write_candidate(wire, gap_after=0.003)

    time.sleep(hold_ms / 1000.0 + 0.15)
    echoed = [parse_adu(link.read_exact(len(wire), 8.0)).wire
              for wire in requests[:RX_BLOCKS]]
    if echoed != requests[:RX_BLOCKS]:
        raise AssertionError("RX pool did not retain the first eight ADUs in order")
    link.assert_silence(0.15)

    sentinel = b"pool-recovered"
    link.echo(0x11, 0x64, sentinel)
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats,
        (
            "response_failures",
            "selftest_failures",
            "uart_rx_overrun",
            "uart_rx_errors",
            "uart_tx_errors",
            "uart_restarts",
            "rtu_crc_errors",
            "rtu_too_short",
            "rtu_oversize",
            "rtu_stream_gaps",
            "rtu_send_refused_busy",
            "rtu_send_failed",
            "rx_rejected",
            "tx_exhausted",
            "tx_rejected",
        ),
    )
    refused = len(requests) - RX_BLOCKS
    assert_fields(
        stats,
        echo_frames=RX_BLOCKS + 1,
        echo_data_bytes=RX_BLOCKS * len(pool_data(0)) + len(sentinel),
        control_frames=2,
        rtu_candidates=len(requests) + 3,
        rtu_frames_received=RX_BLOCKS + 3,
        rtu_allocation_failure=refused,
        rtu_frames_sent=RX_BLOCKS + 2,
        rx_exhausted=refused,
    )
    return {
        "suite": "pool",
        "sent": len(requests),
        "retained": len(echoed),
        "stats": stats,
    }


def stress_data(sequence: int, size: int) -> bytes:
    if size < 4:
        return bytes((sequence + index * 17) & 0xFF for index in range(size))
    state = (sequence ^ (size << 16) ^ 0x9E3779B9) & 0xFFFFFFFF
    data = bytearray(struct.pack("<I", sequence))
    while len(data) < size:
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        data.append(state & 0xFF)
    return bytes(data)


def suite_stress(link: HardwareLink, seconds: float) -> dict:
    link.reset_metrics()
    sizes = (0, 1, 2, 31, 32, 63, 64, 127, 128, 251, 252)
    functions = (0x03, 0x10, 0x43, 0x64, 0xA7)
    sequence = 0
    data_bytes = 0
    started = time.monotonic()
    deadline = started + seconds
    while time.monotonic() < deadline:
        size = sizes[sequence % len(sizes)]
        data = stress_data(sequence, size)
        function = functions[sequence % len(functions)]
        link.echo(1 + (sequence % 247), function, data, timeout=8.0)
        data_bytes += size
        sequence += 1

    elapsed = time.monotonic() - started
    stats = link.stats()
    healthy_failures(stats)
    assert_plain_accounting(stats, sequence, data_bytes)
    counters = stats["counters"]
    cycles = sum(
        counters[name]["total"]
        for name in (
            "usart_irq",
            "rx_dma_irq",
            "tx_dma_irq",
            "uart_slow",
            "packet_process",
            "rtu_tx_release",
        )
    )
    board_seconds = max(stats["window_ms"], 1) / 1000.0
    cpu_percent = 100.0 * cycles / (600_000_000.0 * board_seconds)
    return {
        "suite": "stress",
        "frames": sequence,
        "data_bytes": data_bytes,
        "seconds": elapsed,
        "data_mib_s": data_bytes / elapsed / (1024 * 1024),
        "integrated_cpu_percent": cpu_percent,
        "stats": stats,
    }


def suite_smoke(link: HardwareLink) -> dict:
    link.reset_metrics()
    data = b"MODBUS-H7S\x00smoke"
    link.echo(0x11, 0xA7, data)
    stats = link.stats()
    healthy_failures(stats)
    assert_plain_accounting(stats, 1, len(data))
    return {"suite": "smoke", "stats": stats}


def print_result(result: dict) -> None:
    suite = result["suite"]
    if suite == "stress":
        print(
            f"PASS stress: {result['frames']} frames, {result['data_bytes']} B, "
            f"{result['data_mib_s']:.3f} MiB/s data, "
            f"{result['integrated_cpu_percent']:.3f}% measured CPU"
        )
    elif suite == "vectors":
        print(
            f"PASS vectors: {result['frames']} frames / "
            f"{result['data_bytes']} data bytes"
        )
    elif suite == "pool":
        print(
            f"PASS pool: sent={result['sent']} retained={result['retained']}"
        )
    else:
        print(f"PASS {suite}")


def append_result(path: str | None, baud: int,
                  crc_policy: str, result: dict) -> None:
    if not path:
        return
    record = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "baud": baud,
        "crc_policy": crc_policy,
        "status": result.get("status", "passed"),
        **result,
    }
    with Path(path).open("a", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":"),
                                sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--crc-policy", choices=tuple(CRC_POLICIES), default="bitwise"
    )
    parser.add_argument(
        "--suite",
        choices=("smoke", "vectors", "faults", "selftest", "pool",
                 "stress", "all"),
        default="smoke",
    )
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--output", help="append one compact JSON object per suite")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")

    reference_self_check()
    print("PASS independent PC CRC/ADU oracle")

    with serial.Serial(args.port, args.baud, timeout=0.02,
                       write_timeout=10) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        port.reset_output_buffer()
        link = HardwareLink(port, CRC_POLICIES[args.crc_policy])
        hello = link.hello()
        print("HELLO", json.dumps(hello, sort_keys=True))

        suites = {
            "smoke": lambda: suite_smoke(link),
            "vectors": lambda: suite_vectors(link),
            "faults": lambda: suite_faults(link),
            "selftest": lambda: suite_selftest(link),
            "pool": lambda: suite_pool(link),
            "stress": lambda: suite_stress(link, args.seconds),
        }
        selected = ("vectors", "faults", "selftest", "pool", "stress") \
            if args.suite == "all" else (args.suite,)
        for name in selected:
            try:
                result = suites[name]()
            except Exception as error:
                failure = {
                    "suite": name,
                    "status": "failed",
                    "error": str(error),
                }
                try:
                    failure["stats"] = link.stats()
                except Exception as stats_error:
                    failure["stats_error"] = str(stats_error)
                append_result(args.output, args.baud, args.crc_policy, failure)
                print("FAIL", json.dumps(failure, sort_keys=True))
                raise
            print_result(result)
            append_result(args.output, args.baud, args.crc_policy, result)


if __name__ == "__main__":
    main()
