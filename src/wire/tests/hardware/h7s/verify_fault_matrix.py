#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Check the fixed live fault matrix, intentional error counts and restoration."""
from collections import Counter
import argparse
import json
import math
from pathlib import Path
import sys

from run_fault_matrix import REPO, SRC, digest, plan

sys.path[:0] = [str(SRC / "wire/tests"), str(SRC / "cobs/tests/hardware/h7s"),
                str(SRC / "modbus/rtu/tests/hardware/h7s")]
from provenance import Provenance
import cobs_hardware as cobs
import modbus_hardware as rtu


def close(actual, expected):
    assert math.isfinite(actual) and math.isclose(actual, expected, rel_tol=1e-11, abs_tol=1e-11), (actual, expected)


def expected_suites(phase):
    suites = dict(vectors=1, faults=1, selftest=1, pool=1, stress=1 + bool(phase["extended_seconds"]))
    if phase["protocol"] == "cobs":
        suites["gap"] = 1
    else:
        suites.update(crc_benchmark=1, paced=1 + bool(phase["extended_seconds"]))
    return Counter(suites)


def check_cobs(phase, row):
    assert row["crc"] == phase["policy"] and row["max_payload"] == phase["maximum"]
    width = 0 if phase["policy"] == "none" else 2
    header = 1 if phase["maximum"] + width <= 255 else 2
    assert row["length_size"] == header
    stats, suite = row["stats"], row["suite"]
    cobs.assert_observation_occupancy(stats)
    cobs.assert_zero(stats, "response_failures", "selftest_failures", "uart_rx_errors",
                     "uart_tx_errors", "uart_restarts", "rx_rejected", "tx_rejected", "cobs_send_failed")
    if suite != "gap":
        cobs.assert_zero(stats, "uart_rx_overrun")
    if suite == "faults":
        oversize = int(phase["maximum"] + width < (1 << (8 * header)) - 1)
        cases = 6 + oversize + bool(width)
        assert row["cases"] == cases
        cobs.assert_fields(stats, cobs_malformed=1, cobs_oversize=oversize, cobs_length_mismatch=4,
                           cobs_crc_errors=int(bool(width)), cobs_frames_lost=cases - 1, cobs_resyncs=2,
                           echo_frames=cases, control_frames=1, cobs_frames_delivered=cases + 1)
    elif suite == "selftest":
        cobs.assert_fields(stats, cobs_send_refused_busy=1, tx_exhausted=1, echo_frames=0)
    elif suite == "pool":
        assert (row["sent"], row["echoed"]) == (32, 8)
        cobs.assert_fields(stats, cobs_frames_lost=24, cobs_allocation_failure=24, rx_exhausted=24,
                           cobs_resyncs=24, echo_frames=9)
    elif suite == "gap":
        assert stats["uart_rx_overrun"] > 0 and stats["cobs_frames_lost"] > 0 and stats["cobs_resyncs"] > 0
        cobs.assert_fields(stats, control_frames=2, cobs_frames_delivered=stats["echo_frames"] + 2,
                           cobs_frames_sent=stats["echo_frames"] + 1)
    else:
        cobs.healthy_failures(stats)
        cobs.assert_plain_echo_accounting(stats, row["frames"], row["payload_bytes"])
        if suite == "stress":
            assert row["frames"] > 0
            cycles = sum(stats["counters"][name]["total"] for name in
                         ("usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow", "packet_process", "cobs_tx_release"))
            close(row["integrated_cpu_percent"], cycles / (6000 * stats["window_ms"]))


def check_rtu(phase, row):
    assert row["crc_policy"] == phase["policy"] and not row["framer"]
    policy = rtu.CRC_POLICIES[phase["policy"]]
    image = row["image"]
    assert image["policy"] == phase["policy"] and image["policy_id"] == policy.identifier
    assert image["baud"] == phase["baud"] and not image["framer"]
    assert image["optimization"] == "-Os" and not image["lto"]
    assert image["lookup_bytes"] == (256 * policy.wire_size if phase["policy"].endswith("table") else 0)
    assert not image["probe_helper_calls"]
    suite = row["suite"]
    if suite == "crc_benchmark":
        assert [m["size"] for m in row["measurements"]] == [0, 1, 8, 32, 64, 128, 246, 256]
        for measurement in row["measurements"]:
            size = measurement["size"]
            checksum = policy.calculate(bytes((i * 37 + 0xA5) & 255 for i in range(size)))
            assert len(measurement["samples"]) == 9
            for sample in measurement["samples"]:
                assert sample["size"] == size and sample["iterations"] == 8
                assert sample["checksum"] == checksum
                assert sample["checksum_mix"] == sum(checksum ^ i for i in range(8)) & ((1 << 64) - 1)
                assert 0 < sample["cycles"] < 600000
        return
    stats = row["stats"]
    rtu.assert_observation_occupancy(stats)
    rtu.assert_zero(stats, ("response_failures", "selftest_failures", "uart_rx_overrun", "uart_rx_errors",
                           "uart_tx_errors", "uart_restarts", "rx_rejected", "tx_rejected", "rtu_send_failed"))
    if suite == "faults":
        rtu.assert_fields(stats, echo_frames=4, rtu_crc_errors=4 if policy.wire_size else 0,
                          rtu_frames_received=5, rtu_candidates=9 if policy.wire_size else 5)
        assert row["corruptions" if policy.wire_size else "accepted_corruptions"] == 4
    elif suite == "selftest":
        rtu.assert_fields(stats, rtu_send_refused_busy=1, tx_exhausted=1, echo_frames=0)
    elif suite == "pool":
        assert (row["sent"], row["retained"]) == (16, 8)
        rtu.assert_fields(stats, rtu_allocation_failure=8, rx_exhausted=8, echo_frames=9)
    else:
        rtu.healthy_failures(stats)
        rtu.assert_plain_accounting(stats, row["frames"], row["data_bytes"])
        if suite in ("stress", "paced"):
            assert row["frames"] > 0
            cycles = sum(stats["counters"][name]["total"] for name in
                         ("usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow", "packet_process", "rtu_tx_release"))
            assert row["instrumented_cycles"] == cycles
            close(row["integrated_cpu_percent"], cycles / (6000 * stats["window_ms"]))
            close(row["cycles_per_frame"], cycles / row["frames"])


def verify(folder, local_images=False):
    receipt = json.loads((folder / "session.json").read_text(encoding="utf-8"))
    assert receipt["schema"] == 1 and receipt["completed"] and receipt["restored_and_verified"]
    assert receipt["backup_bytes"] == 65536 and receipt["backup_sha256"] == receipt["readback_sha256"]
    assert receipt["plan"] == plan(), "the requested full matrix must not be narrowed or relabelled"
    assert [p["tag"] for p in receipt["phases"]] == [p["tag"] for p in plan()], "missing/reordered phases"
    provenance = Provenance(REPO)
    provenance.check(receipt["source_base_commit"], {"src/wire/tests/hardware/h7s/run_fault_matrix.py": receipt["runner_sha256"]})
    totals = Counter()
    session = Path(receipt["session"])
    rtu.FRAMED = False
    for phase, entry in zip(receipt["plan"], receipt["phases"]):
        assert entry["completed"] and entry["exit_code"] == 0 and entry["flash_verified"]
        assert entry["results"] == phase["tag"] + ".jsonl"
        path = folder / entry["results"]
        assert digest(path) == entry["results_sha256"], f"altered result {path}"
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
        assert Counter(r["suite"] for r in rows) == expected_suites(phase), f"incomplete suites for {phase['tag']}"
        for row in rows:
            assert row["baud"] == phase["baud"] and row.get("status", "passed") == "passed"
            if phase["protocol"] == "cobs":
                assert row["elf_sha256"] == entry["elf_sha256"]
                provenance.check(row["source_base_commit"], row["source_sha256"])
                check_cobs(phase, row)
            else:
                provenance.check(row["image"]["source_base_commit"], row["image"]["source_sha256"])
                check_rtu(phase, row)
        if local_images:
            assert digest(session / (phase["tag"] + ".elf")) == entry["elf_sha256"]
            log = session / (phase["tag"] + ".log")
            assert digest(log) == entry["log_sha256"]
            assert "Download verified successfully" in log.read_text(encoding="utf-8", errors="replace")
            if phase["protocol"] == "rtu":
                assert all(digest(session / (phase["tag"] + ".bin")) == r["image"]["binary_sha256"] for r in rows)
        totals[phase["protocol"]] += len(rows)
        print(f"PASS {phase['tag']}: {len(rows)} suites, exact negative counters and recovered ownership")
    if local_images:
        assert digest(session / "before.bin") == digest(session / "after.bin") == receipt["backup_sha256"]
    provenance.report()
    print(f"PASS {len(receipt['phases'])} images; {totals['cobs']} COBS + {totals['rtu']} RTU suite records; original firmware restored")
    if not local_images:
        print("CAVEAT retained ELFs, flash logs and backup bytes not re-read (use --local-images on the measuring machine)")
    return totals


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__:
        parser.error("run without python -O")
    verify(args.folder, args.local_images)
