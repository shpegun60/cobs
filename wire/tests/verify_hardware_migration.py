#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Recheck shared-policy hardware evidence, source identities and CPU arithmetic."""
from collections import Counter
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys

REPO = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def identity(sources):
    for relative, expected in sources.items():
        actual = hashlib.sha256((REPO / relative).read_bytes()).hexdigest()
        assert actual == expected, f"source changed since hardware run: {relative}"


def rows(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def close(actual, expected):
    assert math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12), (actual, expected)


def verify():
    cobs_dir = REPO / "cobs/tests/hardware/h7s"
    cobs = module("cobs_hardware", cobs_dir / "cobs_hardware.py")
    counter_names = ("usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow", "packet_process")
    for filename, policy, maximum, expected_count in (
        ("results_crc_default_2026-09-05.jsonl", "bitwise", 253, 29),
        ("results_crc_table_2026-09-05.jsonl", "table", 253, 28),
        ("results_legacy_shared_storage_2026-09-05.jsonl", "none", 1024, 28),
    ):
        records = rows(cobs_dir / filename)
        assert len(records) == expected_count
        assert records[-1]["suite"] == "smoke" and records[-1]["baud"] == 115200
        for baud in (115200, 1000000, 3000000, 6000000, 10000000):
            suites = Counter(r["suite"] for r in records if r["baud"] == baud)
            for suite in ("vectors", "faults", "selftest", "pool", "stress"):
                assert suites[suite] >= 1, (filename, baud, suite)
        for r in records:
            assert r["crc"] == policy and r["max_payload"] == maximum
            identity(r["source_sha256"])
            stats = r["stats"]
            if r["suite"] in ("vectors", "stress", "smoke"):
                cobs.healthy_failures(stats)
            if r["suite"] == "stress":
                cobs.assert_plain_echo_accounting(stats, r["frames"], r["payload_bytes"])
                total = sum(stats["counters"][n]["total"] for n in (*counter_names, "cobs_tx_release"))
                close(r["integrated_cpu_percent"], 100 * total / (600000000 * stats["window_ms"] / 1000))
                close(r["payload_mib_s"], r["payload_bytes"] / r["seconds"] / 2**20)
        stress = next(r for r in records if r["suite"] == "stress" and r["baud"] == 1000000)
        print(f"COBS {policy}/{maximum}: {len(records)} records; 1M {stress['frames']} frames, "
              f"{stress['payload_bytes']} B, {stress['integrated_cpu_percent']:.6f}% CPU")

    rtu_dir = REPO / "modbus/rtu/tests/hardware/h7s"
    rtu = module("modbus_hardware", rtu_dir / "modbus_hardware.py")
    records = rows(rtu_dir / "results_shared_storage_2026-09-05.jsonl")
    assert len(records) == 127 and all(r["status"] == "passed" for r in records)
    assert records[-1]["suite"] == "smoke" and records[-1]["baud"] == 115200
    expected_suites = Counter(dict.fromkeys(("vectors", "faults", "selftest", "pool", "crc_benchmark", "stress", "paced"), 1))
    policies = ("nocrc", "crc8-bitwise", "crc8-table", "crc16-bitwise", "crc16-table",
                "crc32-bitwise", "crc32-table", "crc64-bitwise", "crc64-table")
    for name in policies:
        for baud in (115200, 1000000):
            group = [r for r in records if r["crc_policy"] == name and r["baud"] == baud]
            assert Counter(r["suite"] for r in group) == expected_suites
            image = group[0]["image"]
            assert all(r["image"] == image for r in group)
            identity(image["source_sha256"])
            policy = rtu.CRC_POLICIES[name]
            assert image["policy_id"] == policy.identifier
            assert image["lookup_bytes"] == (256 * policy.wire_size if name.endswith("table") else 0)
            assert not image["probe_helper_calls"]
            for r in group:
                if r["suite"] in ("stress", "paced"):
                    stats = r["stats"]
                    rtu.healthy_failures(stats)
                    rtu.assert_plain_accounting(stats, r["frames"], r["data_bytes"])
                    total = sum(stats["counters"][n]["total"] for n in (*counter_names, "rtu_tx_release"))
                    assert total == r["instrumented_cycles"]
                    close(r["integrated_cpu_percent"], 100 * total / (600000000 * stats["window_ms"] / 1000))
                    close(r["cycles_per_frame"], total / r["frames"])
                    close(r["normalized_cpu_percent_300fps"], 100 * total / r["frames"] * 300 / 600000000)
                if r["suite"] == "crc_benchmark":
                    for measurement in r["measurements"]:
                        size = measurement["size"]
                        expected = policy.calculate(bytes((i * 37 + 0xA5) & 255 for i in range(size)))
                        assert all(s["checksum"] == expected for s in measurement["samples"])
        group = [r for r in records if r["crc_policy"] == name and r["baud"] == 1000000]
        benchmark = next(r for r in group if r["suite"] == "crc_benchmark")
        timing = next(m for m in benchmark["measurements"] if m["size"] == 246)
        paced = next(r for r in group if r["suite"] == "paced")
        print(f"RTU {name}: {timing['cycles_per_byte']:.6f} cycles/B; "
              f"paced {paced['integrated_cpu_percent']:.6f}% CPU; "
              f"{image['lookup_bytes']} table B, {image['probe_static_instructions']} static instructions")

    arm = json.loads((REPO / "crc/tests/results_shared_policies_arm_2026-09-05.json").read_text())
    assert arm["passed"] == 6360 and arm["failed"] == 0 and len(arm["cpus"]) == 106
    identity(arm["source_sha256"])
    print("PASS 85 COBS + 127 RTU live records and 6360 ARM objects; current source hashes and derived CPU metrics match")


if __name__ == "__main__":
    verify()
