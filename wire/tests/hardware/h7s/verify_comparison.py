#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Independently verify paired live observations and derive comparison tables."""
import argparse
from contextlib import redirect_stdout
import hashlib
import io
import json
import math
from pathlib import Path
import statistics
import subprocess

import run_comparison as bench


def close(a, b):
    assert math.isfinite(a) and math.isclose(a, b, rel_tol=1e-11, abs_tol=1e-11), (a, b)


def core_summary(group):
    samples = group["samples"]
    components = {phase: statistics.median(s[phase + "_cycles"] / s["iterations"] for s in samples)
                  for phase in ("rx", "tx", "release")}
    totals = [(s["rx_cycles"] + s["tx_cycles"] + s["release_cycles"]) / s["iterations"] for s in samples]
    return dict(**components, total=statistics.median(totals), minimum=min(totals), maximum=max(totals))


def verify(data, core_only=False):
    assert data["schema"] == 1 and data["status"] == "passed" and data["restored_and_verified"]
    missing = []
    for path, digest in data["source_sha256"].items():
        absolute = bench.REPO / path
        if path.startswith("stm32_cube_test/") and not absolute.exists():
            missing.append(path); continue
        assert hashlib.sha256(absolute.read_bytes()).hexdigest() == digest, f"changed source {path}"
    assert {c["policy"] for c in data["core"]} == set(bench.POLICIES) and len(data["core"]) == 3
    core = {}
    for run in data["core"]:
        assert run["header"]["core_clock"] == 600000000
        for group in run["groups"]:
            key = (group["protocol"], group["policy"], group["size"], group["pattern"], group["chunk"])
            assert key not in core
            body = bench.payload(group["size"], bench.PATTERNS.index(group["pattern"]))
            assert hashlib.sha256(body).hexdigest() == group["payload_sha256"]
            encoded = bench.wire(group["protocol"], group["policy"], body, group["maximum"])
            assert encoded.hex() == group["wire_hex"] and len(encoded) == group["wire_bytes"]
            assert len(group["samples"]) == 9
            for index, s in enumerate(group["samples"]):
                assert s["index"] == index and s["iterations"] == (1 if group["size"] == 1024 else 4)
                assert 0 < s["rx_cycles"] + s["tx_cycles"] + s["release_cycles"] <= s["irq_off_cycles"] < 600000
            core[key] = group
    expected = {(proto, policy, size, pattern, chunk) for policy in bench.POLICIES
                for size in bench.SIZES for pattern in bench.PATTERNS
                for proto, chunk in (("cobs", 0), ("cobs", 128), ("rtu", 0))}
    assert set(core) == expected
    uart = {}
    for run in data["uart"]:
        for row in run["rows"]:
            key = (row["protocol"], row["policy"], row["baud"], row["case"], row["repeat"])
            assert key not in uart
            case = next(c for c in bench.CASES if c[0] == row["case"])
            bodies = bench.corpus(case)
            frames = tuple(bench.wire(row["protocol"], row["policy"], body) for body in bodies)
            rate = bench.target_fps(case, row["baud"])
            assert row["target_fps"] == rate and row["frames"] == rate * 2
            n = row["frames"]
            assert row["data_bytes"] == sum(len(bodies[i % len(bodies)]) for i in range(n))
            assert row["wire_bytes_per_direction"] == sum(len(frames[i % len(frames)]) for i in range(n))
            digest = hashlib.sha256()
            for body in bodies:
                digest.update(len(body).to_bytes(4, "little")); digest.update(body)
            assert digest.hexdigest() == row["corpus_sha256"]
            s = row["stats"]
            if row["protocol"] == "cobs":
                bench.cobs.healthy_failures(s)
                bench.cobs.assert_plain_echo_accounting(s, n, row["data_bytes"])
                release = "cobs_tx_release"
            else:
                bench.rtu.healthy_failures(s)
                bench.rtu.assert_plain_accounting(s, n, row["data_bytes"])
                release = "rtu_tx_release"
            names = ("usart_irq", "rx_dma_irq", "tx_dma_irq", "uart_slow", "packet_process", release)
            cycles = sum(s["counters"][name]["total"] for name in names)
            assert cycles == row["instrumented_cycles"]
            close(row["cycles_per_echo"], cycles / n)
            close(row["cpu_percent"], cycles / (6000 * s["window_ms"]))
            close(row["normalized_cpu_percent"], cycles / n * rate / 6000000)
            close(row["actual_wire_percent"], row["wire_bytes_per_direction"] * 1000000 / (row["baud"] * s["window_ms"]))
            uart[key] = row
    if not core_only:
        expected_uart = {(proto, policy, baud, case[0], repeat) for proto in ("cobs", "rtu")
                         for policy in bench.POLICIES for baud in (115200, 1000000)
                         for case in bench.CASES for repeat in (0, 1)}
        assert set(uart) == expected_uart
        assert {(p["protocol"], p["baud"]) for p in data["probes"]} == {
            (proto, baud) for proto in ("cobs", "rtu") for baud in (3000000, 6000000, 10000000)}
        for probe in data["probes"]:
            exact = len(probe["trials"]) == 12 and all(t["exact"] for t in probe["trials"])
            assert probe["all_exact"] == exact
            if probe["protocol"] == "cobs":
                assert exact, "unexpected COBS high-baud framing failure"
            for trial in probe["trials"]:
                assert trial["expected"] == bench.wire(probe["protocol"], "bitwise", bench.payload(trial["size"], 0)).hex()
                assert trial["exact"] == (trial["received"] == trial["expected"])
    return core, uart, missing


def verify_images(data, nm):
    session = Path(data["session"])
    images = []
    for run in data["core"]:
        images.append((f"core-{run['policy']}-115200", run["policy"], run["elf_sha256"]))
    for run in (*data["uart"], *data["probes"]):
        images.append((f"{run['protocol']}-{run['policy']}-{run['baud']}", run["policy"], run["elf_sha256"]))
    for name, policy, digest in images:
        elf = session / (name + ".elf")
        assert hashlib.sha256(elf.read_bytes()).hexdigest() == digest
        table = [s.split(maxsplit=3) for s in subprocess.check_output(
            [str(nm), "-S", "-C", "--defined-only", str(elf)], text=True).splitlines() if "::lookup_" in s]
        assert sum(int(s[1], 16) for s in table) == (512 if policy == "table" else 0)
        objdump = nm.with_name(nm.name.replace("-nm", "-objdump"))
        sections = [s.split() for s in subprocess.check_output(
            [str(objdump), "-t", "-C", str(elf)], text=True).splitlines() if "::lookup_" in s]
        assert len(sections) == len(table) and all(".rodata" in s for s in sections)
        assert "Download verified successfully" in (session / (name + "-flash.log")).read_text(encoding="utf-8", errors="replace")
    assert hashlib.sha256((session / "before.bin").read_bytes()).hexdigest() == data["backup_sha256"]
    assert "Download verified successfully" in (session / "restore.log").read_text(encoding="utf-8", errors="replace")
    print(f"PASS {len(images)} exact flashed ELF images / private lookup ROM / restored backup proof")


def tables(core, uart, probes):
    print("\n### Library-only RX + TX + release, random 252-byte payload\n")
    print("| CRC | COBS whole cycles | COBS chunk128 cycles | RTU whole cycles | COBS whole / RTU |")
    print("|---|---:|---:|---:|---:|")
    for policy in bench.POLICIES:
        totals = [core_summary(core[(proto, policy, 252, "random", chunk)])["total"]
                  for proto, chunk in (("cobs", 0), ("cobs", 128), ("rtu", 0))]
        print(f"| {policy} | {totals[0]:.1f} | {totals[1]:.1f} | {totals[2]:.1f} | {totals[0] / totals[2]:.2f}x |")
    print("\n### Equal packet-rate CPU model from measured library-only cycles, random 252 bytes\n")
    print("| Nominal baud | COBS NoCrc % | RTU NoCrc % | COBS Bitwise % | RTU Bitwise % | COBS Table % | RTU Table % |")
    print("|---:|---:|---:|---:|---:|---:|---:|")
    # Equal useful throughput: the same reference 257-byte frame cadence is
    # applied to BOTH protocols, not two different saturated wire byte rates.
    reference_wire = len(bench.wire("cobs", "bitwise", bench.payload(252, 0)))
    for baud in (115200, 1000000, 3000000, 6000000, 10000000):
        fps = baud / (10 * reference_wire)
        values = [core_summary(core[(proto, policy, 252, "random", 0)])["total"] * fps / 6000000
                  for policy in bench.POLICIES for proto in ("cobs", "rtu")]
        print(f"| {baud} | " + " | ".join(f"{v:.3f}" for v in values) + " |")
    if uart:
        print("\n### Actual UART echo, random252, equal scheduled rate\n")
        print("| Baud | Frames/s | COBS NoCrc % | RTU NoCrc % | COBS Bitwise % | RTU Bitwise % | COBS Table % | RTU Table % |")
        print("|---:|---:|---:|---:|---:|---:|---:|---:|")
        for baud in (115200, 1000000):
            values = []
            for policy in bench.POLICIES:
                for proto in ("cobs", "rtu"):
                    rows = [uart[(proto, policy, baud, "random252", r)] for r in (0, 1)]
                    values.append(sum(r["instrumented_cycles"] for r in rows) / (6000 * sum(r["stats"]["window_ms"] for r in rows)))
            print(f"| {baud} | {bench.target_fps(bench.CASES[3], baud)} | " + " | ".join(f"{v:.3f}" for v in values) + " |")
        print("\n### Actual UART echo at 1M, Bitwise, all scenarios\n")
        print("| Case | Frames/s | COBS CPU % | RTU CPU % | COBS cycles/echo | RTU cycles/echo |")
        print("|---|---:|---:|---:|---:|---:|")
        for case in bench.CASES:
            metrics = []
            for proto in ("cobs", "rtu"):
                rows = [uart[(proto, "bitwise", 1000000, case[0], r)] for r in (0, 1)]
                cycles = sum(r["instrumented_cycles"] for r in rows)
                metrics.append((cycles / (6000 * sum(r["stats"]["window_ms"] for r in rows)), cycles / sum(r["frames"] for r in rows)))
            print(f"| {case[0]} | {bench.target_fps(case, 1000000)} | {metrics[0][0]:.3f} | {metrics[1][0]:.3f} | {metrics[0][1]:.1f} | {metrics[1][1]:.1f} |")
    if probes:
        print("\n### High-baud physical framing probes (exact echoes / 12)\n")
        print("| Protocol | Baud | 8 B | 32 B | 128 B | 252 B | Total exact echoes |")
        print("|---|---:|---:|---:|---:|---:|---:|")
        for p in probes:
            counts = []
            for size in (8, 32, 128, 252):
                trials = [t for t in p["trials"] if t["size"] == size]
                counts.append(f"{sum(t['exact'] for t in trials)}/{len(trials)}" if trials else "not run")
            name = "COBS" if p["protocol"] == "cobs" else "RTU"
            print(f"| {name} | {p['baud']} | " + " | ".join(counts) +
                  f" | {sum(t['exact'] for t in p['trials'])}/{len(p['trials'])} |")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--core-only", action="store_true")
    parser.add_argument("--nm", type=Path)
    parser.add_argument("--check-doc", type=Path)
    args = parser.parse_args()
    data = json.loads(args.results.read_text(encoding="utf-8"))
    core, uart, missing = verify(data, args.core_only)
    print(f"PASS {len(core)} core groups / {sum(len(g['samples']) for g in core.values())} windows; {len(uart)} UART rows; {len(data['probes'])} framing probes; restored firmware")
    if missing: print(f"CAVEAT {len(missing)} ignored Cube source files unavailable")
    if args.nm: verify_images(data, args.nm)
    if args.check_doc:
        output = io.StringIO()
        with redirect_stdout(output): tables(core, uart, data["probes"])
        rows = {line for line in output.getvalue().splitlines() if line.startswith("|")}
        published = set(args.check_doc.read_text(encoding="utf-8").splitlines())
        assert rows <= published, f"missing/stale document rows: {rows - published}"
        print(f"PASS {len(rows)} distinct comparison table/header rows in document")
    tables(core, uart, data["probes"])


if __name__ == "__main__": main()
