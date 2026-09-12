#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Render/check the report tables directly from independently verified receipts."""
import argparse
import json
from pathlib import Path
from statistics import median
import verify as v

START = "<!-- heap-crc-measurements:start -->"
END = "<!-- heap-crc-measurements:end -->"


def render(summaries, receipt):
    images = {(s["protocol"], s["policy"]): s for s in summaries}
    assert set(images) == {(p, c) for p in (0, 1) for c in range(4)}, "full report needs all images"
    def name(p): return "RTU" if p else "COBS"
    def core(p, c, m, n, scenario=0):
        return next(r["total"]["median"] for r in images[p, c]["core"]
                    if (r["memory"], r["size"], r["pattern"], r["scenario"]) == (m, n, 0, scenario))
    def uart(p, c, m, case, field):
        return median(r[field] for r in images[p, c]["uart"] if r["memory"] == m and r["case"] == case)
    lines = [START, "## Measured results", "",
        "Source: [complete live receipt](../src/wire/tests/hardware/h7s/heap_crc/results_2026-09-12/session.json). "
        "Tables are regenerated and checked by `report.py --check-doc`.", "",
        "### Packet allocation: known-size 250-byte pseudorandom body", "",
        "Endpoint echo only, excluding UART. Time includes receive, transmit construction and release. "
        "One microsecond is 600 core cycles on this board.", "",
        "| Protocol | CRC | Pool cycles | Heap cycles | Heap delta |",
        "|---|---|---:|---:|---:|"]
    for p in (0, 1):
        for c in (0, 1, 2, 3):
            pool, heap = core(p, c, 0, 250), core(p, c, 1, 250)
            lines.append(f"| {name(p)} | {v.METHODS[c]} | {pool:.1f} | {heap:.1f} | {100 * (heap / pool - 1):+.2f}% |")
    lines += ["", "### Growth and fragmentation with the STM32 CRC", "",
        "Cycles per endpoint echo, pseudorandom body. Grow means a zero-capacity hint and "
        "16-byte appends; it includes the extra append calls as well as allocations/copies. "
        "Fragmented means the specified 48-live-block layout, still with a known-size hint.", "",
        "| Protocol | Body bytes | Pool known | Heap known | Pool grow | Heap grow | Heap fragmented |",
        "|---|---:|---:|---:|---:|---:|---:|"]
    for p in (0, 1):
        for n in (8, 250, 1024):
            vals = [core(p, 3, m, n, scenario) for m, scenario in ((0, 0), (1, 0), (0, 1), (1, 1), (1, 2))]
            lines.append(f"| {name(p)} | {n} | " + " | ".join(f"{x:.1f}" for x in vals) + " |")
    lines += ["", "### CRC calculation alone", "",
        "Cycles per calculation including setup/reset, offset 0, warm AXI SRAM, "
        "medians of nine samples from the COBS images. NoCrc performs no checksum calculation "
        "and therefore has no raw-calculator row. Other offsets and all sizes remain in the receipt.", "",
        "| Input bytes | Bitwise | Table | STM32 | Bitwise / STM32 | Table / STM32 |",
        "|---|---:|---:|---:|---:|---:|"]
    for n in (0, 1, 4, 8, 32, 128, 250, 1024, 4096):
        b, t, h = [next(r["cycles"]["median"] for r in images[0, c]["crc"] if (r["size"], r["offset"]) == (n, 0)) for c in (1, 2, 3)]
        lines.append(f"| {n} | {b:.2f} | {t:.2f} | {h:.2f} | {b / h:.2f}x | {t / h:.2f}x |")
    lines += ["", "### Live UART: 250-byte bodies at nominal 1 Mbaud", "",
        "Pseudorandom and zero-filled frames alternate. Values are medians of two windows. "
        "The rate column spans all four Pool/Heap windows in that row. CPU is measured "
        "communication work only, at the observed VCP-paced delivery rate, not full system load.", "",
        "| Protocol | CRC | Pool CPU | Heap CPU | Pool cycles/echo | Heap cycles/echo | Delivered kB/s |",
        "|---|---|---:|---:|---:|---:|---:|"]
    for p in (0, 1):
        for c in (0, 1, 2, 3):
            cpu = [uart(p, c, m, "long250", "cpu") for m in (0, 1)]
            cycles = [uart(p, c, m, "long250", "cycles_per_echo") for m in (0, 1)]
            rates = [r["payload_bytes_per_second"] / 1000 for r in images[p, c]["uart"] if r["case"] == "long250"]
            lines.append(f"| {name(p)} | {v.METHODS[c]} | {cpu[0]:.3f}% | {cpu[1]:.3f}% | {cycles[0]:.1f} | {cycles[1]:.1f} | {min(rates):.2f}-{max(rates):.2f} |")
    lines += ["", "### Live UART: short/mixed traffic with the STM32 CRC", "",
        "Different delivery rates make CPU percentages across these cases incomparable on their own.", "",
        "| Protocol | Body pattern | Pool CPU | Heap CPU | Pool cycles/echo | Heap cycles/echo | Pool / Heap kB/s |",
        "|---|---|---:|---:|---:|---:|---:|"]
    for p in (0, 1):
        for case in ("short8", "mixed"):
            cpu = [uart(p, 3, m, case, "cpu") for m in (0, 1)]
            cycles = [uart(p, 3, m, case, "cycles_per_echo") for m in (0, 1)]
            rates = [uart(p, 3, m, case, "payload_bytes_per_second") / 1000 for m in (0, 1)]
            lines.append(f"| {name(p)} | {case} | {cpu[0]:.3f}% | {cpu[1]:.3f}% | {cycles[0]:.1f} | {cycles[1]:.1f} | {rates[0]:.2f} / {rates[1]:.2f} |")
    echoes = sum(r["received"] for i in receipt["images"] for r in i["uart"])
    lines += ["", f"Full session: **8 images, 5,184 endpoint timing records, 1,728 raw CRC timing records, "
        f"16,400 hardware/software CRC comparisons, 96 UART windows, {echoes:,} exact echoes**. "
        "No recorded protocol, allocation-lifecycle or UART errors. The original 65,536 flash bytes "
        "were restored and independently read back; backup/read-back SHA-256: "
        f"`{receipt['backup_sha256']}`.", END]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--check-doc", type=Path)
    parser.add_argument("--local-images", action="store_true")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    summaries = v.verify_session(args.directory, args.local_images)
    receipt = json.loads((args.directory / "session.json").read_text())
    text = render(summaries, receipt)
    if args.check_doc:
        doc = args.check_doc.read_text(encoding="utf-8")
        assert doc.count(START) == doc.count(END) == 1 and text in doc, "report tables drifted from receipts"
        print("PASS all five report tables match independent calculations")
    else:
        print(text)


if __name__ == "__main__":
    main()
