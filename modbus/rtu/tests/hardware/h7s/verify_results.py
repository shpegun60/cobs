#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""Recheck the accepted nine-policy/1M benchmark from its raw evidence."""

from collections import Counter
import argparse
import json
import math
from pathlib import Path
import statistics

from modbus_hardware import (
    CRC_POLICIES, assert_plain_accounting, healthy_failures,
)

COUNTERS = (
    "usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow",
    "packet_process", "rtu_tx_release",
)


def close(actual, expected):
    if not math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12):
        raise AssertionError(f"derived metric mismatch: {actual} != {expected}")


def verify(path: Path) -> None:
    rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    assert len(rows) == 82 and all(row["status"] == "passed" for row in rows)
    assert rows[-1]["suite"] == "smoke" and rows[-1]["baud"] == 115200
    assert CRC_POLICIES[rows[-1]["crc_policy"]].identifier == 0
    paced_payloads = set()
    manifest_hashes = set()
    for name, policy in list(CRC_POLICIES.items())[:9]:
        group = [row for row in rows if row["crc_policy"] == name and row["baud"] == 1000000]
        assert Counter(row["suite"] for row in group) == {
            "vectors": 1, "faults": 1, "selftest": 1, "pool": 1,
            "crc_benchmark": 1, "stress": 2, "paced": 2,
        }, name
        images = [row["image"] for row in group]
        assert all(image == images[0] for image in images)
        image = images[0]
        assert image["policy_id"] == policy.identifier and image["baud"] == 1000000
        assert image["optimization"] == "-Os" and not image["lto"]
        assert image["lookup_bytes"] == (256 * policy.wire_size if name.endswith("table") else 0)
        assert (image["data_bytes"], image["bss_bytes"]) == (12, 6864)
        assert not image["probe_helper_calls"]
        manifest_hashes.add(image["binary_sha256"])
        for row in group:
            if row["suite"] == "crc_benchmark":
                assert [m["size"] for m in row["measurements"]] == [0, 1, 8, 32, 64, 128, 246, 256]
                for measurement in row["measurements"]:
                    samples = measurement["samples"]
                    assert len(samples) == 9
                    size = measurement["size"]
                    checksum = policy.calculate(bytes((i * 37 + 0xA5) & 255 for i in range(size)))
                    times = []
                    for sample in samples:
                        assert sample["size"] == size and sample["iterations"] == 8
                        assert 0 < sample["cycles"] < 600_000
                        assert sample["checksum"] == checksum
                        assert sample["checksum_mix"] == sum(checksum ^ i for i in range(8)) & ((1 << 64) - 1)
                        assert sample["dwt_ctrl"] & 1 and not sample["dwt_ctrl"] & (1 << 25)
                        assert sample["scb_ccr"] & (3 << 16) == 3 << 16
                        assert (sample["scb_cpuid"] >> 4) & 0xFFF == 0xC27
                        times.append(sample["cycles"] / 8)
                    median = statistics.median(times)
                    close(measurement["median_cycles_per_call"], median)
                    close(measurement["min_cycles_per_call"], min(times))
                    close(measurement["max_cycles_per_call"], max(times))
                    if size:
                        close(measurement["cycles_per_byte"], median / size)
                    else:
                        assert measurement["cycles_per_byte"] is None
                    if size and policy.wire_size:
                        close(measurement["calculation_mib_s"], 600_000_000 * size / median / 2**20)
                    else:
                        assert measurement["calculation_mib_s"] is None
            elif row["suite"] in ("stress", "paced"):
                stats = row["stats"]
                healthy_failures(stats)
                assert_plain_accounting(stats, row["frames"], row["data_bytes"])
                cycles = sum(stats["counters"][counter]["total"] for counter in COUNTERS)
                assert row["instrumented_cycles"] == cycles
                close(row["cycles_per_frame"], cycles / row["frames"])
                close(row["integrated_cpu_percent"], 100 * cycles / (600_000_000 * stats["window_ms"] / 1000))
                close(row["normalized_cpu_percent_300fps"], 100 * cycles / row["frames"] * 300 / 600_000_000)
                close(row["frames_s"], row["frames"] / row["seconds"])
                close(row["data_mib_s"], row["data_bytes"] / row["seconds"] / 2**20)
                if row["suite"] == "paced" and row["seconds"] > 10:
                    assert 299 < row["frames_s"] < 301
                    paced_payloads.add((row["frames"], row["data_bytes"]))
    assert len(manifest_hashes) == 9, "all nine policies must have distinct load images"
    assert len(paced_payloads) == 1, "extended paced runs must carry identical useful data"
    print(f"PASS {len(rows)} records: identities, all raw CRC samples, accounting, CPU/rate formulas, equal paced payloads")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    arguments = parser.parse_args()
    if not __debug__:
        parser.error("do not run this assertion-based verifier with python -O")
    verify(arguments.results)
