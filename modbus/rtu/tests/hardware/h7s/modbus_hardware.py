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
import statistics
import struct
import time

import serial


MAX_ADU_SIZE = 256
UART_CHUNK_SIZE = 256
UART_CHUNK_COUNT = 4
RX_BLOCKS = 8
TX_BLOCKS = 2

CONTROL_ADDRESS = 0xF7
CONTROL_FUNCTION = 0x41
MAGIC = b"MRTU"
PROTOCOL_VERSION = 3

CMD_HELLO = 1
CMD_STATS = 2
CMD_RESET = 3
CMD_HOLD = 4
CMD_SELFTEST = 5
CMD_CRC_BENCHMARK = 6

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

@dataclass(frozen=True)
class IntegrityPolicy:
    identifier: int
    wire_size: int
    width: int
    polynomial: int
    initial: int
    xor_out: int
    reflected: bool
    wire_order: str
    check_value: int

    @property
    def max_data_size(self) -> int:
        return MAX_ADU_SIZE - 2 - self.wire_size

    def calculate(self, data: bytes) -> int:
        if self.width == 0:
            return 0
        mask = (1 << self.width) - 1
        value = self.initial
        for byte in data:
            if self.reflected:
                value ^= byte
                for _ in range(8):
                    value = (value >> 1) ^ (
                        self.polynomial if value & 1 else 0
                    )
            else:
                value ^= byte << (self.width - 8)
                for _ in range(8):
                    value = ((value << 1) & mask) ^ (
                        self.polynomial
                        if value & (1 << (self.width - 1)) else 0
                    )
        return (value ^ self.xor_out) & mask

    def append(self, body: bytes) -> bytes:
        if self.wire_size == 0:
            return body
        trailer = self.calculate(body).to_bytes(
            self.wire_size, byteorder=self.wire_order
        )
        return body + trailer

    def verify(self, wire: bytes) -> bool:
        if len(wire) < self.wire_size:
            return False
        if self.wire_size == 0:
            return True
        body = wire[:-self.wire_size]
        received = int.from_bytes(
            wire[-self.wire_size:], byteorder=self.wire_order
        )
        return received == self.calculate(body)


CRC16_BITWISE = IntegrityPolicy(
    0, 2, 16, 0xA001, 0xFFFF, 0, True, "little", 0x4B37
)
CRC16_TABLE = IntegrityPolicy(
    1, 2, 16, 0xA001, 0xFFFF, 0, True, "little", 0x4B37
)
NO_CRC = IntegrityPolicy(2, 0, 0, 0, 0, 0, False, "little", 0)
CRC8_BITWISE = IntegrityPolicy(
    3, 1, 8, 0x07, 0, 0, False, "big", 0xF4
)
CRC8_TABLE = IntegrityPolicy(
    4, 1, 8, 0x07, 0, 0, False, "big", 0xF4
)
CRC32_BITWISE = IntegrityPolicy(
    5, 4, 32, 0xEDB88320, 0xFFFFFFFF, 0xFFFFFFFF,
    True, "little", 0xCBF43926
)
CRC32_TABLE = IntegrityPolicy(
    6, 4, 32, 0xEDB88320, 0xFFFFFFFF, 0xFFFFFFFF,
    True, "little", 0xCBF43926
)
CRC64_BITWISE = IntegrityPolicy(
    7, 8, 64, 0x42F0E1EBA9EA3693, 0, 0,
    False, "big", 0x6C40DF5F0B497347
)
CRC64_TABLE = IntegrityPolicy(
    8, 8, 64, 0x42F0E1EBA9EA3693, 0, 0,
    False, "big", 0x6C40DF5F0B497347
)

CRC_POLICIES = {
    "crc16-bitwise": CRC16_BITWISE,
    "crc16-table": CRC16_TABLE,
    "nocrc": NO_CRC,
    "crc8-bitwise": CRC8_BITWISE,
    "crc8-table": CRC8_TABLE,
    "crc32-bitwise": CRC32_BITWISE,
    "crc32-table": CRC32_TABLE,
    "crc64-bitwise": CRC64_BITWISE,
    "crc64-table": CRC64_TABLE,
    # Backward-compatible command-line spellings.
    "bitwise": CRC16_BITWISE,
    "table": CRC16_TABLE,
}

# Identical application payload distribution for every performance run. The
# widest trailer (CRC64) still leaves 246 useful bytes in the 256-byte ADU.
STRESS_DATA_SIZES = (0, 1, 2, 31, 32, 63, 64, 127, 128, 245, 246)


def make_adu(policy: IntegrityPolicy, address: int,
             function: int, data: bytes = b"") -> bytes:
    if not 0 <= address <= 0xFF:
        raise ValueError("address must fit one byte")
    if not 0 <= function <= 0xFF:
        raise ValueError("function must fit one byte")
    if len(data) > policy.max_data_size:
        raise ValueError("function data exceeds the selected policy limit")
    body = bytes((address, function)) + data
    return policy.append(body)


@dataclass(frozen=True)
class Adu:
    address: int
    function: int
    data: bytes
    wire: bytes


def parse_adu(policy: IntegrityPolicy, wire: bytes) -> Adu:
    minimum = 2 + policy.wire_size
    if not minimum <= len(wire) <= MAX_ADU_SIZE:
        raise AssertionError(f"invalid ADU size from board: {len(wire)}")
    if not policy.verify(wire):
        raise AssertionError("board response integrity trailer mismatch")
    data_end = len(wire) - policy.wire_size \
        if policy.wire_size else len(wire)
    return Adu(wire[0], wire[1], wire[2:data_end], wire)


def reference_self_check(policy: IntegrityPolicy) -> None:
    if policy.calculate(b"123456789") != policy.check_value:
        raise AssertionError("PC integrity oracle failed its named check value")
    known = bytes((0x01, 0x03, 0x00, 0x00, 0x00, 0x0A))
    known_adu = make_adu(policy, 0x01, 0x03, known[2:])
    if parse_adu(policy, known_adu).wire != known_adu:
        raise AssertionError("PC ADU parser rejected its own canonical vector")
    if policy.wire_size:
        for byte_index in range(len(known_adu)):
            for bit in range(8):
                damaged = bytearray(known_adu)
                damaged[byte_index] ^= 1 << bit
                try:
                    parse_adu(policy, bytes(damaged))
                except AssertionError:
                    continue
                raise AssertionError(
                    "PC integrity oracle accepted a single-bit mutation"
                )
    for size in range(policy.max_data_size + 1):
        data = bytes((index * 37 + size) & 0xFF for index in range(size))
        wire = make_adu(policy, 0x11, 0xA7, data)
        parsed = parse_adu(policy, wire)
        if parsed.data != data or len(wire) != size + 2 + policy.wire_size:
            raise AssertionError(f"PC ADU round trip failed at data size {size}")


class HardwareLink:
    def __init__(self, port: serial.Serial, policy: IntegrityPolicy):
        self.port = port
        self.token = 0
        self.policy = policy

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
        request = make_adu(self.policy, address, function, data)
        self.write_candidate(request)
        response = parse_adu(
            self.policy, self.read_exact(len(request), timeout)
        )
        if response.wire != request:
            raise AssertionError(
                "echo mismatch: "
                f"sent={request.hex()} received={response.wire.hex()}"
            )
        return response

    def echo_wire(self, request: bytes) -> None:
        """Timed traffic uses a precomputed independent-oracle wire image."""
        self.write_candidate(request)
        response = self.read_exact(len(request), timeout=8.0)
        if response != request:
            raise AssertionError(
                f"echo mismatch: sent={request.hex()} received={response.hex()}"
            )

    def control(self, command: int, payload_size: int,
                arguments: tuple[int, ...] = (),
                timeout: float = 5.0) -> bytes:
        self.token = (self.token + 1) & 0xFFFFFFFF
        data = MAGIC + bytes((command,)) + struct.pack("<I", self.token)
        for argument in arguments:
            data += struct.pack("<I", argument)
        request = make_adu(
            self.policy, CONTROL_ADDRESS, CONTROL_FUNCTION, data
        )
        self.write_candidate(request)
        expected_data_size = 9 + payload_size
        response = parse_adu(
            self.policy,
            self.read_exact(
                expected_data_size + 2 + self.policy.wire_size, timeout
            ),
        )
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
            "max_receive": self.policy.max_data_size,
            "max_send": self.policy.max_data_size,
            "max_frame": MAX_ADU_SIZE,
            "uart_chunk_size": UART_CHUNK_SIZE,
            "uart_chunk_count": UART_CHUNK_COUNT,
            "rx_blocks": RX_BLOCKS,
            "tx_blocks": TX_BLOCKS,
            "crc_policy": self.policy.identifier,
        }
        if hello != expected:
            raise AssertionError(f"unexpected board geometry: {hello}")
        return hello

    def ack(self, command: int, argument: int | None = None,
            timeout: float = 5.0) -> tuple[int, int]:
        arguments = () if argument is None else (argument,)
        payload = self.control(command, 8, arguments, timeout)
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

    def crc_benchmark(self, size: int = 246,
                      iterations: int = 8) -> dict:
        if not 0 <= size <= 256:
            raise ValueError("CRC benchmark size must be in 0..256")
        if not 1 <= iterations <= 8:
            raise ValueError("CRC benchmark iterations must be in 1..8")
        payload = self.control(
            CMD_CRC_BENCHMARK, 48, (size, iterations), timeout=10.0
        )
        status, returned_size, returned_iterations, cycles, checksum_mix, \
            returned_checksum, dwt_ctrl, ccr, cpuid = struct.unpack(
                "<IIIQQQIII", payload
            )
        if status != 0 or returned_size != size or \
                returned_iterations != iterations:
            raise AssertionError(
                "CRC benchmark response mismatch: "
                f"status={status} size={returned_size} "
                f"iterations={returned_iterations}"
            )

        data = bytes(
            (index * 37 + 0xA5) & 0xFF
            for index in range(size)
        )
        checksum = self.policy.calculate(data)
        expected_mix = sum(checksum ^ iteration for iteration in range(iterations)) \
            & 0xFFFFFFFFFFFFFFFF
        if returned_checksum != checksum or checksum_mix != expected_mix:
            raise AssertionError(
                f"CRC benchmark oracle mismatch: checksum={returned_checksum:#x} "
                f"expected={checksum:#x}, mix={checksum_mix:#x}/{expected_mix:#x}"
            )
        if not (dwt_ctrl & 1) or dwt_ctrl & (1 << 25):
            raise AssertionError("DWT cycle counter is not available/enabled")
        if ccr & (3 << 16) != (3 << 16):
            raise AssertionError("benchmark requires both I-cache and D-cache")
        if (cpuid >> 4) & 0xFFF != 0xC27:
            raise AssertionError(f"benchmark target is not Cortex-M7: {cpuid:#x}")
        if not 0 < cycles < 600_000:
            raise AssertionError(f"invalid or >= 1 ms IRQ-off cycle window: {cycles}")
        return {
            "size": size,
            "iterations": iterations,
            "cycles": cycles,
            "checksum": checksum,
            "checksum_mix": checksum_mix,
            "dwt_ctrl": dwt_ctrl,
            "scb_ccr": ccr,
            "scb_cpuid": cpuid,
        }


def suite_crc_benchmark(link: HardwareLink) -> dict:
    measurements = []
    for size in (0, 1, 8, 32, 64, 128, 246, 256):
        samples = [link.crc_benchmark(size, 8) for _ in range(9)]
        per_call = [sample["cycles"] / sample["iterations"] for sample in samples]
        median = statistics.median(per_call)
        measurements.append({
            "size": size,
            "median_cycles_per_call": median,
            "min_cycles_per_call": min(per_call),
            "max_cycles_per_call": max(per_call),
            "cycles_per_byte": median / size if size else None,
            # NoCrc does not read input: timings are a harness floor, not
            # fictitious memory/checksum throughput.
            "calculation_mib_s": (
                600_000_000.0 * size / median / (1024 * 1024)
                if size and link.policy.wire_size else None
            ),
            "samples": samples,
        })
    return {"suite": "crc_benchmark", "measurements": measurements}


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
    sizes = tuple(sorted(set(
        size for size in (
            0, 1, 2, 31, 32, 63, 64, 127, 128,
            link.policy.max_data_size - 1,
            link.policy.max_data_size,
        )
        if 0 <= size <= link.policy.max_data_size
    )))
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
    base = make_adu(link.policy, 1, 0x03, b"\x00\x10\x00\x02")
    mutation_indices = (0, 1, 2, len(base) - 1)

    if link.policy.wire_size == 0:
        for byte_index in mutation_indices:
            damaged = bytearray(base)
            damaged[byte_index] ^= 0x01
            link.write_candidate(bytes(damaged))
            echoed = parse_adu(
                link.policy, link.read_exact(len(damaged), 8.0)
            )
            if echoed.wire != bytes(damaged):
                raise AssertionError("NoCrc did not echo altered data verbatim")
        stats = link.stats()
        healthy_failures(stats)
        accepted = len(mutation_indices)
        assert_fields(
            stats,
            echo_frames=accepted,
            echo_data_bytes=accepted * 4,
            control_frames=1,
            rtu_candidates=accepted + 1,
            rtu_frames_received=accepted + 1,
            rtu_crc_errors=0,
            rtu_frames_sent=accepted,
        )
        return {
            "suite": "faults",
            "accepted_corruptions": accepted,
            "stats": stats,
        }

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

    requests = [
        make_adu(link.policy, 0x11, 0x43, pool_data(index))
        for index in range(16)
    ]
    for wire in requests:
        # This explicit silence is part of the v1 physical-burst contract.
        link.write_candidate(wire, gap_after=0.003)

    time.sleep(hold_ms / 1000.0 + 0.15)
    echoed = [parse_adu(link.policy, link.read_exact(len(wire), 8.0)).wire
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


def suite_stress(link: HardwareLink, seconds: float,
                 frame_rate: float = 0) -> dict:
    sizes = STRESS_DATA_SIZES
    functions = (0x03, 0x10, 0x43, 0x64, 0xA7)
    # Outside both the wall-clock interval and the board metric baseline.
    # Every length occurs equally often; no Python CRC runs in the timed loop.
    requests = [make_adu(
        link.policy, 1 + (index % 247), functions[index % len(functions)],
        stress_data(index, sizes[index % len(sizes)]),
    ) for index in range(len(sizes) * 100)]
    link.reset_metrics()
    sequence = 0
    data_bytes = 0
    max_lateness = 0.0
    started = time.monotonic()
    deadline = started + seconds
    while time.monotonic() < deadline:
        if frame_rate:
            scheduled = started + sequence / frame_rate
            if scheduled >= deadline:
                break
            remaining = scheduled - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            max_lateness = max(max_lateness, time.monotonic() - scheduled)
        size = sizes[sequence % len(sizes)]
        link.echo_wire(requests[sequence % len(requests)])
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
        "suite": "paced" if frame_rate else "stress",
        "target_frames_s": frame_rate or None,
        "frames_s": sequence / elapsed,
        "max_schedule_lateness_ms": 1000 * max_lateness if frame_rate else None,
        "frames": sequence,
        "data_bytes": data_bytes,
        "seconds": elapsed,
        "data_mib_s": data_bytes / elapsed / (1024 * 1024),
        "integrated_cpu_percent": cpu_percent,
        "instrumented_cycles": cycles,
        "cycles_per_frame": cycles / sequence,
        "normalized_cpu_percent_300fps": 100 * cycles / sequence * 300 / 600_000_000,
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
    if suite in ("stress", "paced"):
        print(
            f"PASS {suite}: {result['frames']} frames, {result['data_bytes']} B, "
            f"{result['data_mib_s']:.3f} MiB/s data, "
            f"{result['integrated_cpu_percent']:.3f}% measured CPU"
        )
    elif suite == "crc_benchmark":
        measurement = next(m for m in result["measurements"] if m["size"] == 246)
        print(
            f"PASS CRC benchmark: 8 lengths x 9 samples x 8 calls; 246 B: "
            f"{measurement['median_cycles_per_call']:.1f} cycles/call, "
            f"{measurement['cycles_per_byte']:.3f} cycles/B (gross)"
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
                  crc_policy: str, result: dict, image: dict | None = None) -> None:
    if not path:
        return
    record = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "baud": baud,
        "crc_policy": crc_policy,
        "status": result.get("status", "passed"),
        "image": image,
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
                 "crc_benchmark", "stress", "paced", "all"),
        default="smoke",
    )
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--frame-rate", type=float, default=300.0)
    parser.add_argument("--image", help="manifest of the exact inspected/flashed ELF")
    parser.add_argument("--output", help="append one compact JSON object per suite")
    args = parser.parse_args()
    if args.seconds <= 0 or args.frame_rate <= 0:
        parser.error("--seconds and --frame-rate must be positive")

    policy = CRC_POLICIES[args.crc_policy]
    image = json.loads(Path(args.image).read_text(encoding="utf-8")) \
        if args.image else None
    if image and (image["policy_id"] != policy.identifier or image["baud"] != args.baud):
        parser.error("image manifest does not match selected policy/baud")
    reference_self_check(policy)
    print("PASS independent PC integrity/ADU oracle")

    with serial.Serial(args.port, args.baud, timeout=0.02,
                       write_timeout=10) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        port.reset_output_buffer()
        link = HardwareLink(port, policy)
        hello = link.hello()
        print("HELLO", json.dumps(hello, sort_keys=True))

        suites = {
            "smoke": lambda: suite_smoke(link),
            "vectors": lambda: suite_vectors(link),
            "faults": lambda: suite_faults(link),
            "selftest": lambda: suite_selftest(link),
            "pool": lambda: suite_pool(link),
            "crc_benchmark": lambda: suite_crc_benchmark(link),
            "stress": lambda: suite_stress(link, args.seconds),
            "paced": lambda: suite_stress(link, args.seconds, args.frame_rate),
        }
        selected = (
            "vectors", "faults", "selftest", "pool",
            "crc_benchmark", "stress", "paced",
        ) \
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
                append_result(args.output, args.baud, args.crc_policy, failure, image)
                print("FAIL", json.dumps(failure, sort_keys=True))
                raise
            print_result(result)
            append_result(args.output, args.baud, args.crc_policy, result, image)


if __name__ == "__main__":
    main()
