#!/usr/bin/env python3
"""Independent PC-side driver for the H7S COBS + UART hardware harness."""

from __future__ import annotations

import argparse
from collections import deque
import json
import random
import struct
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.exit("pyserial required: pip install pyserial")


MAGIC = bytes((0xC7, 0x43, 0x42, 0x53))
MAX_PAYLOAD = 1024
RX_BLOCKS = 8
TX_BLOCKS = 2
UART_CHUNK_SIZE = 128
UART_CHUNK_COUNT = 8
PROTOCOL_VERSION = 1

CMD_HELLO = 1
CMD_STATS = 2
CMD_RESET = 3
CMD_HOLD = 4
CMD_STALL = 5
CMD_SELFTEST = 6

STATS_FIELDS = (
    "version",
    "window_ms",
    "echo_frames",
    "echo_bytes",
    "control_frames",
    "response_failures",
    "selftest_failures",
    "uart_rx_overrun",
    "uart_rx_errors",
    "uart_tx_errors",
    "uart_restarts",
    "cobs_frames_delivered",
    "cobs_frames_lost",
    "cobs_allocation_failure",
    "cobs_malformed",
    "cobs_oversize",
    "cobs_length_mismatch",
    "cobs_resyncs",
    "cobs_frames_sent",
    "cobs_send_refused_busy",
    "cobs_send_failed",
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
    "uart_rx",
    "uart_slow",
    "uart_tx_start",
    "cobs_consume",
    "cobs_tx_release",
    "packet_process",
)


def cobs_encode(raw: bytes) -> bytes:
    """Reference encoder matching the canonical no-redundant-FF ending."""
    out = bytearray((0,))
    code_pos = 0
    code = 1
    last_block_was_ff = False

    for byte in raw:
        if byte != 0:
            out.append(byte)
            code += 1
            if code != 0xFF:
                continue
        out[code_pos] = code
        last_block_was_ff = code == 0xFF
        code_pos = len(out)
        out.append(0)
        code = 1

    if code == 1 and last_block_was_ff:
        del out[code_pos:]
    else:
        out[code_pos] = code
    return bytes(out)


def cobs_decode(encoded: bytes) -> bytes:
    if not encoded:
        raise ValueError("empty COBS encoding")
    if 0 in encoded:
        raise ValueError("zero inside COBS encoding")
    out = bytearray()
    pos = 0
    while pos < len(encoded):
        code = encoded[pos]
        end = pos + code
        if end > len(encoded) + 1:
            raise ValueError("COBS block exceeds frame")
        data_end = min(end, len(encoded))
        if data_end - (pos + 1) != code - 1:
            raise ValueError("truncated COBS block")
        out.extend(encoded[pos + 1:data_end])
        pos = end
        if code != 0xFF and pos < len(encoded):
            out.append(0)
    return bytes(out)


def wire_from_raw(raw: bytes) -> bytes:
    return cobs_encode(raw) + b"\x00"


def engine_frame(body: bytes) -> bytes:
    if len(body) > MAX_PAYLOAD:
        raise ValueError("body exceeds hardware format")
    return wire_from_raw(struct.pack("<H", len(body)) + body)


def decode_engine(encoded: bytes) -> bytes:
    raw = cobs_decode(encoded)
    if len(raw) < 2:
        raise ValueError("engine length header absent")
    declared = struct.unpack_from("<H", raw)[0]
    body = raw[2:]
    if declared != len(body):
        raise ValueError(f"engine length mismatch: {declared} != {len(body)}")
    if declared > MAX_PAYLOAD:
        raise ValueError("board returned oversized body")
    return body


def reference_self_check() -> None:
    """Pin the PC oracle to known COBS and engine-wire vectors."""
    known = {
        b"": bytes.fromhex("01"),
        b"\x00": bytes.fromhex("0101"),
        b"\x11\x22\x00\x33": bytes.fromhex("0311220233"),
        bytes(range(1, 255)): bytes((0xFF,)) + bytes(range(1, 255)),
    }
    for raw, expected in known.items():
        encoded = cobs_encode(raw)
        if encoded != expected or cobs_decode(encoded) != raw:
            raise AssertionError(f"PC COBS oracle failed a {len(raw)}-byte vector")

    for size in (0, 1, 2, 253, 254, 255, 256, 508, 509, 1024, 1025):
        for name in ("zero", "nonzero", "alternating", "boundary", "random"):
            raw = pattern(name, size, 0x52454600 ^ size)
            if cobs_decode(cobs_encode(raw)) != raw:
                raise AssertionError(f"PC COBS oracle failed {name}/{size}")

    if engine_frame(b"") != bytes.fromhex("01010100"):
        raise AssertionError("PC engine oracle failed the empty-body vector")
    if engine_frame(b"\x11") != bytes.fromhex("0201021100"):
        raise AssertionError("PC engine oracle failed the one-byte vector")

    for malformed in (b"", b"\x00", b"\x03\x11", b"\x02\x00"):
        try:
            cobs_decode(malformed)
        except ValueError:
            continue
        raise AssertionError(f"PC COBS oracle accepted malformed {malformed.hex()}")


class HardwareLink:
    def __init__(self, port: serial.Serial):
        self.port = port
        self.rx = bytearray()
        self.frames: deque[bytes] = deque()
        self.token = 0

    def send_body(self, body: bytes) -> None:
        self.port.write(engine_frame(body))

    def write_raw(self, data: bytes) -> None:
        self.port.write(data)

    def _pump(self) -> None:
        count = max(self.port.in_waiting, 1)
        chunk = self.port.read(min(count, 65536))
        if not chunk:
            return
        self.rx.extend(chunk)
        while True:
            try:
                end = self.rx.index(0)
            except ValueError:
                return
            encoded = bytes(self.rx[:end])
            del self.rx[:end + 1]
            if not encoded:  # bare delimiter is a synchronization no-op
                continue
            self.frames.append(decode_engine(encoded))

    def read_body(self, timeout: float = 5.0) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.frames:
                return self.frames.popleft()
            self._pump()
        raise TimeoutError(
            f"no COBS response (partial={len(self.rx)} bytes, queued={len(self.frames)})"
        )

    def drain(self, seconds: float) -> list[bytes]:
        deadline = time.monotonic() + seconds
        found: list[bytes] = []
        while time.monotonic() < deadline:
            self._pump()
            while self.frames:
                found.append(self.frames.popleft())
        return found

    def echo(self, body: bytes, timeout: float = 5.0) -> None:
        self.send_body(body)
        self.port.flush()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            received = self.read_body(max(0.05, deadline - time.monotonic()))
            if received == body:
                return
        raise AssertionError(f"echo not returned for {len(body)} bytes")

    def control(self, command: int, argument: int | None = None,
                timeout: float = 5.0) -> bytes:
        self.token = (self.token + 1) & 0xFFFFFFFF
        request = MAGIC + bytes((command,)) + struct.pack("<I", self.token)
        if argument is not None:
            request += struct.pack("<I", argument)
        self.send_body(request)
        self.port.flush()

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            body = self.read_body(max(0.05, deadline - time.monotonic()))
            if (len(body) >= 9 and body[:4] == MAGIC and
                    body[4] == (command | 0x80) and
                    struct.unpack_from("<I", body, 5)[0] == self.token):
                return body[9:]
        raise AssertionError(f"control response missing for command {command}")

    def hello(self) -> dict[str, int]:
        payload = self.control(CMD_HELLO)
        if len(payload) != 40:
            raise AssertionError(f"bad HELLO size: {len(payload)}")
        names = (
            "version", "baud", "core_clock", "max_receive", "max_send",
            "length_size", "uart_chunk_size", "uart_chunk_count",
            "rx_blocks", "tx_blocks",
        )
        hello = dict(zip(names, struct.unpack("<10I", payload), strict=True))
        if hello != {
            "version": PROTOCOL_VERSION,
            "baud": self.port.baudrate,
            "core_clock": 600_000_000,
            "max_receive": 1024,
            "max_send": 1024,
            "length_size": 2,
            "uart_chunk_size": UART_CHUNK_SIZE,
            "uart_chunk_count": UART_CHUNK_COUNT,
            "rx_blocks": RX_BLOCKS,
            "tx_blocks": TX_BLOCKS,
        }:
            raise AssertionError(f"unexpected board geometry: {hello}")
        return hello

    def ack(self, command: int, argument: int | None = None,
            timeout: float = 5.0) -> tuple[int, int]:
        payload = self.control(command, argument, timeout)
        if len(payload) != 8:
            raise AssertionError(f"bad ACK size for {command}: {len(payload)}")
        return struct.unpack("<2I", payload)

    def reset_metrics(self) -> None:
        status, _ = self.ack(CMD_RESET)
        if status != 0:
            raise AssertionError(f"metric reset refused: {status}")
        # The baseline is applied after the ACK's DMA borrow is released.
        time.sleep(0.05)

    def stats(self) -> dict:
        payload = self.control(CMD_STATS)
        scalar_bytes = 4 * len(STATS_FIELDS)
        expected = scalar_bytes + 16 * len(COUNTER_NAMES)
        if len(payload) != expected:
            raise AssertionError(f"bad STATS size: {len(payload)} != {expected}")
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


def safe_body(body: bytes) -> bytes:
    if len(body) >= len(MAGIC) and body[:len(MAGIC)] == MAGIC:
        body = bytes((body[0] ^ 0x01,)) + body[1:]
    return body


def pattern(name: str, size: int, seed: int) -> bytes:
    if name == "zero":
        body = bytes(size)
    elif name == "nonzero":
        body = bytes(1 + (i % 255) for i in range(size))
    elif name == "alternating":
        body = bytes(0 if i % 2 == 0 else 0xA5 for i in range(size))
    elif name == "boundary":
        body = bytes(0 if i % 254 in (0, 253) else (i * 17 + 3) & 0xFF
                     for i in range(size))
    elif name == "random":
        rng = random.Random(seed)
        body = bytes(rng.randrange(256) for _ in range(size))
    else:
        raise ValueError(name)
    return safe_body(body)


def assert_zero(stats: dict, *names: str) -> None:
    bad = {name: stats[name] for name in names if stats[name] != 0}
    if bad:
        raise AssertionError(f"non-zero failure counters: {bad}")


def assert_fields(stats: dict, **expected: int) -> None:
    wrong = {name: (stats[name], value) for name, value in expected.items()
             if stats[name] != value}
    if wrong:
        raise AssertionError(f"counter/accounting mismatch: {wrong}")


def assert_observation_occupancy(stats: dict) -> None:
    # STATS is decoded from a real RX packet.  Its snapshot therefore owns
    # exactly that one RX block, while it is taken before allocating the STATS
    # response's TX block.  Any other occupancy here is a leaked owner.
    assert_fields(
        stats,
        version=PROTOCOL_VERSION,
        rx_available=RX_BLOCKS - 1,
        rx_in_use=1,
        tx_available=TX_BLOCKS,
        tx_in_use=0,
    )


def assert_plain_echo_accounting(stats: dict, frames: int,
                                 payload_bytes: int) -> None:
    assert_fields(
        stats,
        echo_frames=frames,
        echo_bytes=payload_bytes,
        control_frames=1,  # the observing STATS request itself
        cobs_frames_delivered=frames + 1,
        cobs_frames_sent=frames,
    )
    for counter in (
        "packet_process", "uart_tx_start", "tx_dma_irq", "cobs_tx_release"
    ):
        if stats["counters"][counter]["calls"] != frames:
            raise AssertionError(
                f"{counter} call accounting mismatch: "
                f"{stats['counters'][counter]['calls']} != {frames}"
            )


def healthy_failures(stats: dict) -> None:
    assert_observation_occupancy(stats)
    assert_zero(
        stats,
        "response_failures", "selftest_failures",
        "uart_rx_overrun", "uart_rx_errors", "uart_tx_errors", "uart_restarts",
        "cobs_frames_lost", "cobs_allocation_failure", "cobs_malformed",
        "cobs_oversize", "cobs_length_mismatch", "cobs_resyncs",
        "cobs_send_refused_busy", "cobs_send_failed",
        "rx_exhausted", "rx_rejected", "tx_exhausted", "tx_rejected",
    )


def suite_vectors(link: HardwareLink) -> dict:
    link.reset_metrics()
    sizes = (0, 1, 2, 31, 32, 63, 64, 127, 128, 253, 254, 255,
             256, 511, 512, 1023, 1024)
    names = ("zero", "nonzero", "alternating", "boundary", "random")
    frames = 0
    payload_bytes = 0
    started = time.monotonic()
    for size in sizes:
        selected = ("zero",) if size == 0 else names
        for index, name in enumerate(selected):
            body = pattern(name, size, 0xC0B50000 ^ size ^ index)
            link.echo(body, timeout=6.0)
            frames += 1
            payload_bytes += size
    stats = link.stats()
    healthy_failures(stats)
    assert_plain_echo_accounting(stats, frames, payload_bytes)
    return {
        "suite": "vectors",
        "frames": frames,
        "payload_bytes": payload_bytes,
        "seconds": time.monotonic() - started,
        "stats": stats,
    }


def suite_faults(link: HardwareLink) -> dict:
    link.reset_metrics()
    invalid = (
        b"\x00",                                      # bare delimiter/no-op
        b"\x01\x00",                                # no decoded header
        b"\x03\x11\x00",                           # zero while block owes data
        wire_from_raw(struct.pack("<H", 2) + b"\xAA"),
        wire_from_raw(struct.pack("<H", 1) + b"\xAA\xBB"),
        wire_from_raw(struct.pack("<H", 0) + b"\xAA"),
        wire_from_raw(struct.pack("<H", 1025)),
    )
    for index, raw in enumerate(invalid):
        link.write_raw(raw)
        link.port.flush()
        time.sleep(0.02)
        link.echo(b"fault-recovery:" + struct.pack("<I", index))
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats, "response_failures", "selftest_failures",
        "uart_rx_overrun", "uart_rx_errors", "uart_tx_errors", "uart_restarts",
        "cobs_allocation_failure", "cobs_send_failed",
        "rx_exhausted", "rx_rejected", "tx_exhausted", "tx_rejected",
    )
    expected = {
        "cobs_malformed": 1,
        "cobs_oversize": 1,
        "cobs_length_mismatch": 4,
        "cobs_frames_lost": 6,
        "cobs_resyncs": 2,
    }
    wrong = {name: (stats[name], value) for name, value in expected.items()
             if stats[name] != value}
    if wrong:
        raise AssertionError(f"fault classification mismatch: {wrong}")
    assert_fields(
        stats,
        echo_frames=len(invalid),
        control_frames=1,
        cobs_frames_delivered=len(invalid) + 1,
        cobs_frames_sent=len(invalid),
    )
    return {"suite": "faults", "cases": len(invalid), "stats": stats}


def suite_selftest(link: HardwareLink) -> dict:
    link.reset_metrics()
    status, _ = link.ack(CMD_SELFTEST)
    if status != 0:
        raise AssertionError(f"self-test command refused: {status}")
    time.sleep(0.05)
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats, "response_failures", "selftest_failures",
        "uart_rx_overrun", "uart_rx_errors", "uart_tx_errors", "uart_restarts",
        "cobs_frames_lost", "cobs_allocation_failure", "cobs_malformed",
        "cobs_oversize", "cobs_length_mismatch", "cobs_resyncs",
        "cobs_send_failed", "rx_exhausted", "rx_rejected", "tx_rejected",
    )
    if stats["cobs_send_refused_busy"] != 1 or stats["tx_exhausted"] != 1:
        raise AssertionError(
            "backpressure self-test did not hit exactly one Busy and one exhaustion: "
            f"busy={stats['cobs_send_refused_busy']} exhausted={stats['tx_exhausted']}"
        )
    assert_fields(
        stats,
        echo_frames=0,
        control_frames=2,
        cobs_frames_delivered=2,
        cobs_frames_sent=1,
    )
    return {"suite": "selftest", "stats": stats}


def pool_body(index: int) -> bytes:
    prefix = b"POOL" + struct.pack("<I", index)
    return prefix + pattern("random", 64 - len(prefix), 0x504F4F4C ^ index)


def suite_pool(link: HardwareLink) -> dict:
    link.reset_metrics()
    hold_ms = 400
    status, value = link.ack(CMD_HOLD, hold_ms)
    if status != 0 or value != hold_ms:
        raise AssertionError(f"hold refused: {status}/{value}")
    bodies = [pool_body(i) for i in range(32)]
    link.write_raw(b"".join(engine_frame(body) for body in bodies))
    link.port.flush()
    time.sleep(hold_ms / 1000.0 + 0.25)
    echoes = link.drain(0.75)
    echoed = [body for body in echoes if body in bodies]
    sentinel = b"pool-recovered"
    link.echo(sentinel)
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats, "response_failures", "selftest_failures",
        "uart_rx_overrun", "uart_rx_errors", "uart_tx_errors", "uart_restarts",
        "cobs_malformed", "cobs_oversize", "cobs_length_mismatch",
        "cobs_send_failed", "rx_rejected", "tx_exhausted", "tx_rejected",
    )
    if echoed != bodies[:RX_BLOCKS]:
        raise AssertionError(
            f"held FIFO mismatch: got {len(echoed)} packets, expected the first "
            f"{RX_BLOCKS} exactly"
        )
    refused = len(bodies) - RX_BLOCKS
    assert_fields(
        stats,
        echo_frames=RX_BLOCKS + 1,
        echo_bytes=RX_BLOCKS * 64 + len(sentinel),
        control_frames=2,
        cobs_frames_delivered=RX_BLOCKS + 3,
        cobs_frames_lost=refused,
        cobs_allocation_failure=refused,
        cobs_resyncs=refused,
        cobs_frames_sent=RX_BLOCKS + 2,
        rx_exhausted=refused,
    )
    return {
        "suite": "pool",
        "sent": len(bodies),
        "echoed": len(echoed),
        "stats": stats,
    }


def suite_gap(link: HardwareLink) -> dict:
    link.reset_metrics()
    stall_ms = 500
    status, value = link.ack(CMD_STALL, stall_ms)
    if status != 0 or value != stall_ms:
        raise AssertionError(f"stall refused: {status}/{value}")
    # One maximum-size frame is slightly larger than the UART's total 1 KiB
    # chunk pool. Repeating it while the loop is stalled forces physical loss
    # in Uart, not merely an Endpoint allocation refusal.
    body = pattern("random", MAX_PAYLOAD, 0x47415021)
    frame = engine_frame(body)
    flood_frames = 64
    drain_batch = 4
    for first in range(0, flood_frames, drain_batch):
        count = min(drain_batch, flood_frames - first)
        link.write_raw(frame * count)

        # Flood echoes are deliberately irrelevant to this test, but leaving
        # roughly 60 KiB of them unread can overflow the PC-side ST-Link VCP
        # queue at 115200 and manufacture a truncated response. Keep the
        # independent strict decoder active while preventing that host-only
        # loss from being mistaken for a board TX failure.
        link._pump()
        link.frames.clear()
    link.port.flush()
    time.sleep(stall_ms / 1000.0 + 0.35)
    link.drain(0.5)

    # At high baud the entire flood can finish while the main loop is still
    # stalled.  In that ordering Uart reports the gap only after every byte in
    # the drop DMA buffer has already been discarded, so Endpoint correctly
    # waits for the *next* delimiter before trusting framing again.  Supply
    # that synchronization boundary explicitly; the first application frame
    # after a gap is not itself supposed to double as both resync material and
    # deliverable data.
    link.write_raw(b"\x00")
    link.port.flush()
    time.sleep(0.05)
    link.echo(b"uart-gap-recovered", timeout=8.0)
    stats = link.stats()
    assert_observation_occupancy(stats)
    assert_zero(
        stats, "response_failures", "selftest_failures",
        "uart_rx_errors", "uart_tx_errors", "uart_restarts",
        "cobs_malformed", "cobs_oversize", "cobs_length_mismatch",
        "cobs_send_failed", "rx_rejected", "tx_exhausted", "tx_rejected",
    )
    if stats["uart_rx_overrun"] == 0:
        raise AssertionError("physical UART overrun was not induced")
    if stats["cobs_frames_lost"] == 0 or stats["cobs_resyncs"] == 0:
        raise AssertionError("UART gap did not propagate into COBS resync")
    assert_fields(
        stats,
        control_frames=2,
        cobs_frames_delivered=stats["echo_frames"] + 2,
        cobs_frames_sent=stats["echo_frames"] + 1,
    )
    return {"suite": "gap", "stats": stats}


def stress_body(sequence: int, size: int) -> bytes:
    if size < 4:
        raise ValueError("stress body needs a sequence field")
    state = (sequence ^ (size << 16) ^ 0x9E3779B9) & 0xFFFFFFFF
    body = bytearray(struct.pack("<I", sequence))
    while len(body) < size:
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        body.append(state & 0xFF)
    return bytes(body)


def suite_stress(link: HardwareLink, seconds: float, window: int) -> dict:
    link.reset_metrics()
    sizes = (32, 64, 127, 128, 253, 254, 255, 256, 511, 512, 1024)
    pending: deque[bytes] = deque()
    sequence = 0
    payload_bytes = 0
    started = time.monotonic()
    deadline = started + seconds

    while time.monotonic() < deadline or pending:
        while time.monotonic() < deadline and len(pending) < window:
            size = sizes[sequence % len(sizes)]
            body = stress_body(sequence, size)
            link.send_body(body)
            pending.append(body)
            payload_bytes += len(body)
            sequence += 1
        link.port.flush()
        received = link.read_body(timeout=8.0)
        if not pending:
            raise AssertionError("unexpected response with no frame in flight")
        expected = pending.popleft()
        if received != expected:
            raise AssertionError(
                f"stress echo mismatch at sequence {struct.unpack_from('<I', expected)[0]}"
            )

    elapsed = time.monotonic() - started
    stats = link.stats()
    healthy_failures(stats)
    assert_plain_echo_accounting(stats, sequence, payload_bytes)

    cycles = (
        stats["counters"]["usart_irq"]["total"] +
        stats["counters"]["rx_dma_irq"]["total"] +
        stats["counters"]["tx_dma_irq"]["total"] +
        stats["counters"]["uart_slow"]["total"] +
        stats["counters"]["cobs_tx_release"]["total"] +
        stats["counters"]["packet_process"]["total"]
    )
    board_seconds = max(stats["window_ms"], 1) / 1000.0
    cpu_percent = 100.0 * cycles / (600_000_000.0 * board_seconds)
    return {
        "suite": "stress",
        "frames": sequence,
        "payload_bytes": payload_bytes,
        "seconds": elapsed,
        "payload_mib_s": payload_bytes / elapsed / (1024 * 1024),
        "integrated_cpu_percent": cpu_percent,
        "stats": stats,
    }


def print_result(result: dict) -> None:
    suite = result["suite"]
    if suite == "stress":
        print(
            f"PASS stress: {result['frames']} frames, {result['payload_bytes']} B, "
            f"{result['payload_mib_s']:.3f} MiB/s payload, "
            f"{result['integrated_cpu_percent']:.3f}% CPU"
        )
    elif suite == "vectors":
        print(f"PASS vectors: {result['frames']} frames / {result['payload_bytes']} B")
    elif suite == "pool":
        print(f"PASS pool: sent={result['sent']} retained/echoed={result['echoed']}")
    else:
        print(f"PASS {suite}")


def append_result(path: str | None, baud: int, result: dict) -> None:
    if not path:
        return
    record = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "baud": baud,
        **result,
    }
    with Path(path).open("a", encoding="utf-8") as output:
        output.write(json.dumps(record, separators=(",", ":"), sort_keys=True) + "\n")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument(
        "--suite",
        choices=("smoke", "vectors", "faults", "selftest", "pool", "gap",
                 "stress", "all"),
        default="smoke",
    )
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--output", help="append one compact JSON object per suite")
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    if not 1 <= args.window <= 7:
        parser.error("--window must fit the eight-block RX pool (1..7)")

    reference_self_check()
    print("PASS PC reference codec")

    with serial.Serial(args.port, args.baud, timeout=0.02, write_timeout=10) as port:
        time.sleep(0.25)
        port.reset_input_buffer()
        port.reset_output_buffer()
        link = HardwareLink(port)
        hello = link.hello()
        print("HELLO", json.dumps(hello, sort_keys=True))

        if args.suite == "smoke":
            link.reset_metrics()
            link.echo(b"COBS-H7S-smoke\x00ok")
            stats = link.stats()
            healthy_failures(stats)
            assert_plain_echo_accounting(stats, 1, len(b"COBS-H7S-smoke\x00ok"))
            result = {"suite": "smoke", "stats": stats}
            print_result(result)
            append_result(args.output, args.baud, result)
            return

        suites = {
            "vectors": lambda: suite_vectors(link),
            "faults": lambda: suite_faults(link),
            "selftest": lambda: suite_selftest(link),
            "pool": lambda: suite_pool(link),
            "gap": lambda: suite_gap(link),
            "stress": lambda: suite_stress(link, args.seconds, args.window),
        }
        selected = ("vectors", "faults", "selftest", "pool", "stress") \
            if args.suite == "all" else (args.suite,)
        for name in selected:
            result = suites[name]()
            print_result(result)
            append_result(args.output, args.baud, result)


if __name__ == "__main__":
    main()
