#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT
"""Independently check the 300-row live matrix and print weighted Markdown tables."""
from __future__ import annotations

import argparse
from collections import defaultdict
from contextlib import redirect_stdout
from functools import lru_cache
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess

import cobs_performance as perf


def close(actual, expected):
    if not math.isfinite(actual) or not math.isclose(actual, expected, rel_tol=1e-11, abs_tol=1e-11):
        raise AssertionError((actual, expected))


@lru_cache(maxsize=None)
def prepared(policy, maximum, name):
    perf.configure(policy, maximum)
    return perf.corpus(next(c for c in perf.cases(maximum) if c.name == name))


def verify(records, repeats, check_sources=True):
    expected = {(maximum, c.name, baud, policy, repeat)
                for maximum in (253, 1024) for c in perf.cases(maximum)
                for baud in perf.BAUDS for policy in perf.POLICIES for repeat in range(repeats)}
    observed = set()
    identities = set()
    groups = defaultdict(list)
    missing_cube = set()
    for r in records:
        key = (r["max_payload"], r["case"], r["baud"], r["crc"], r["repeat"])
        assert key not in observed, f"duplicate observation {key}"
        observed.add(key)
        assert r["status"] == "passed", r
        assert r["schema"] == 1 and r["suite"] == "matched_performance"
        h = r["hello"]
        assert h["core_clock"] == 600000000 and h["baud"] == r["baud"]
        assert h["max_send"] == h["max_receive"] == r["max_payload"]
        assert h["crc_size"] == (0 if r["crc"] == "none" else 2)
        assert h["length_size"] == (1 if r["max_payload"] == 253 else 2)
        for path, digest in r["source_sha256"].items():
            if not check_sources or (path, digest) in identities:
                continue
            absolute = perf.REPO / path
            if path.startswith("stm32_cube_test/") and not absolute.exists():
                missing_cube.add(path)
                continue
            assert hashlib.sha256(absolute.read_bytes()).hexdigest() == digest, f"changed source {path}"
            identities.add((path, digest))
        bodies, encoded, digest = prepared(r["crc"], r["max_payload"], r["case"])
        assert digest == r["corpus_sha256"] and len(bodies) == r["corpus_frames"]
        counts = [r["frames"] // len(bodies) + int(i < r["frames"] % len(bodies))
                  for i in range(len(bodies))]
        assert counts == r["corpus_counts"]
        assert sum(n * len(b) for n, b in zip(counts, bodies, strict=True)) == r["payload_bytes"]
        assert sum(n * len(b) for n, b in zip(counts, encoded, strict=True)) == r["wire_bytes_per_direction"]
        s = r["stats"]
        perf.peer.healthy_failures(s)
        perf.peer.assert_plain_echo_accounting(s, r["frames"], r["payload_bytes"])
        c = s["counters"]
        for counter in c.values():
            assert counter["avg"] == (counter["total"] // counter["calls"] if counter["calls"] else 0)
            if counter["calls"]:
                assert 0 <= counter["avg"] <= counter["max"] <= counter["total"]
        assert c["cobs_consume"]["total"] <= c["uart_slow"]["total"]
        assert c["uart_tx_start"]["total"] <= c["packet_process"]["total"]
        irq = c["usart_irq"]["total"] + c["rx_dma_irq"]["total"] + c["tx_dma_irq"]["total"]
        thread = c["uart_slow"]["total"] + c["packet_process"]["total"] + c["cobs_tx_release"]["total"]
        total = irq + thread
        assert total == r["instrumented_cycles"]
        assert irq == r["irq_cycles"] and thread == r["thread_inclusive_cycles"]
        seconds = s["window_ms"] / 1000
        assert seconds > 0 and r["host_seconds"] >= r["requested_seconds"]
        close(r["instrumented_cpu_percent"], total / (6000000 * seconds))
        close(r["irq_cpu_percent"], irq / (6000000 * seconds))
        close(r["scope_union_lower_percent"], max(irq, thread) / (6000000 * seconds))
        close(r["scope_union_upper_percent"], r["instrumented_cpu_percent"])
        close(r["rx_consume_cpu_percent"], c["cobs_consume"]["total"] / (6000000 * seconds))
        close(r["packet_process_cpu_percent"], c["packet_process"]["total"] / (6000000 * seconds))
        close(r["cycles_per_echo"], total / r["frames"])
        close(r["cycles_per_payload_byte"], total / r["payload_bytes"])
        close(r["cycles_per_wire_byte"], total / r["wire_bytes_per_direction"])
        close(r["payload_bytes_per_second"], r["payload_bytes"] / seconds)
        close(r["wire_bytes_per_second_per_direction"], r["wire_bytes_per_direction"] / seconds)
        close(r["wire_utilization_percent_per_direction"], r["wire_bytes_per_direction"] * 1000 / (r["baud"] * seconds))
        close(r["extrapolated_full_line_cpu_percent"], total * r["baud"] / (r["wire_bytes_per_direction"] * 60000000))
        assert 0 < r["instrumented_cpu_percent"] < 100
        assert 0 < r["wire_utilization_percent_per_direction"] < 100.1
        groups[(r["max_payload"], r["case"], r["baud"], r["crc"])].append(r)
    assert observed == expected, f"incomplete/unexpected matrix: missing={expected - observed}, extra={observed - expected}"
    for maximum in (253, 1024):
        for case in perf.cases(maximum):
            assert len({r["corpus_sha256"] for r in records
                        if r["max_payload"] == maximum and r["case"] == case.name}) == 1
    return groups, len(identities), missing_cube


def aggregate(rows):
    # Ratios of summed numerators/denominators, NOT averages of row averages.
    seconds = sum(r["stats"]["window_ms"] for r in rows) / 1000
    frames = sum(r["frames"] for r in rows)
    wire = sum(r["wire_bytes_per_direction"] for r in rows)
    cycles = sum(r["instrumented_cycles"] for r in rows)
    baud = rows[0]["baud"]
    return dict(cpu=cycles / (6000000 * seconds), wire=wire * 1000 / (baud * seconds),
                kb_s=wire / seconds / 1000, cpf=cycles / frames,
                extrapolated=cycles * baud / (wire * 60000000),
                cpu_min=min(r["instrumented_cpu_percent"] for r in rows),
                cpu_max=max(r["instrumented_cpu_percent"] for r in rows))


def print_tables(groups):
    for maximum in (253, 1024):
        name = f"random{maximum}"
        print(f"\n### {maximum}-byte random payload, window 7\n")
        print("| Baud | NoCrc CPU % | Bitwise CPU % | Table CPU % | NoCrc wire % | Bitwise wire % | Table wire % |")
        print("|---:|---:|---:|---:|---:|---:|---:|")
        for baud in perf.BAUDS:
            a = [aggregate(groups[(maximum, name, baud, p)]) for p in perf.POLICIES]
            print(f"| {baud} | " + " | ".join(f"{v['cpu']:.3f}" for v in a) + " | " +
                  " | ".join(f"{v['wire']:.1f}" for v in a) + " |")
    print("\n### All cases at 10 Mbaud\n")
    print("| Case | NoCrc CPU % | Bitwise CPU % | Table CPU % | NoCrc cycles/echo | Bitwise cycles/echo | Table cycles/echo |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for maximum in (253, 1024):
        for case in perf.cases(maximum):
            a = [aggregate(groups[(maximum, case.name, 10000000, p)]) for p in perf.POLICIES]
            print(f"| {case.name} | " + " | ".join(f"{v['cpu']:.3f}" for v in a) + " | " +
                  " | ".join(f"{v['cpf']:.0f}" for v in a) + " |")
    print("\n### Linear full-line extrapolation, NOT a saturated-line measurement\n")
    print("| Case | Baud | NoCrc CPU estimate % | Bitwise CPU estimate % | Table CPU estimate % |")
    print("|---|---:|---:|---:|---:|")
    for maximum in (253, 1024):
        for baud in perf.BAUDS:
            a = [aggregate(groups[(maximum, f"random{maximum}", baud, p)]) for p in perf.POLICIES]
            print(f"| random{maximum} | {baud} | " + " | ".join(f"{v['extrapolated']:.2f}" for v in a) + " |")


def check_images(records, session, nm):
    for policy in perf.POLICIES:
        for maximum in (253, 1024):
            for baud in perf.BAUDS:
                group = [r for r in records if (r["crc"], r["max_payload"], r["baud"]) == (policy, maximum, baud)]
                image = session / f"{policy}-{maximum}-{baud}.elf"
                assert {r["elf_sha256"] for r in group} == {hashlib.sha256(image.read_bytes()).hexdigest()}
                symbols = subprocess.check_output([str(nm), "-S", "-C", "--defined-only", str(image)], text=True)
                tables = [line.split(maxsplit=3) for line in symbols.splitlines() if "::lookup_" in line]
                assert sum(int(fields[1], 16) for fields in tables) == (512 if policy == "table" else 0)
                # GNU nm marks a constexpr COMDAT object 'V' (weak object),
                # which says nothing about writability. Check its ELF section.
                objdump = nm.with_name(nm.name.replace("-nm", "-objdump"))
                sections = subprocess.check_output([str(objdump), "-t", "-C", str(image)], text=True)
                lookup_sections = [line.split() for line in sections.splitlines() if "::lookup_" in line]
                assert len(lookup_sections) == len(tables)
                assert all(".rodata" in fields for fields in lookup_sections), lookup_sections
    print("PASS 30 exact ELF identities; NoCrc/Bitwise 0 lookup bytes; Table 512 read-only bytes")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--nm", type=Path, help="optional nm for retained ELF/ROM proof")
    parser.add_argument("--check-doc", type=Path, help="check that this document contains every generated result row")
    args = parser.parse_args()
    records = [json.loads(line) for line in args.results.read_text(encoding="utf-8").splitlines()]
    groups, identities, missing_cube = verify(records, args.repeats)
    receipt = json.loads(Path(str(args.results) + ".session.json").read_text(encoding="utf-8-sig"))
    assert receipt["completed"] and receipt["restored_and_verified"] and receipt["backup_bytes"] == 65536
    assert receipt["results_sha256"] == hashlib.sha256(args.results.read_bytes()).hexdigest()
    print(f"PASS {len(records)} observations; {identities} available source identities; full matrix, corpus, accounting and arithmetic")
    if missing_cube:
        print(f"CAVEAT: {len(missing_cube)} ignored Cube source files unavailable locally")
    if args.nm:
        check_images(records, Path(receipt["session"]), args.nm)
    print(f"PASS firmware restored; {sum(r['frames'] for r in records)} echoes / {sum(r['payload_bytes'] for r in records)} payload bytes")
    if args.check_doc:
        generated = io.StringIO()
        with redirect_stdout(generated):
            print_tables(groups)
        document_lines = set(args.check_doc.read_text(encoding="utf-8").splitlines())
        table_lines = {line for line in generated.getvalue().splitlines() if line.startswith("|")}
        assert table_lines <= document_lines, f"missing/stale document table rows: {table_lines - document_lines}"
        print(f"PASS {len(table_lines)} distinct generated table/header rows in {args.check_doc}")
    print_tables(groups)


if __name__ == "__main__":
    main()
