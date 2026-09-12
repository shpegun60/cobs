#!/usr/bin/env python3
# Author: shpegun60; SPDX-License-Identifier: MIT
"""Independent byte oracle, exact-plan gates and cycle analysis for the H7S run.

No imports from the runner. Assertions are mandatory; Python -O is rejected.
CPU means measured communication work, not a whole-system CPU utilization meter.
"""
import argparse
from collections import Counter
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = next(p for p in HERE.parents if (p / "COBS.pro").is_file())
sys.path.insert(0, str(REPO / "src/wire/tests"))
from provenance import Provenance  # noqa: E402

MAGIC = b"\xb6HC\xa5"
CRC_SIZES = (0, 1, 2, 3, 4, 7, 8, 16, 32, 64, 128, 250, 252, 256, 1024, 4096)
SIZES = ((0, 0), (0, 8), (0, 32), (0, 128), (0, 250), (1, 1024))
METHODS = ("NoCrc", "Bitwise", "Table", "STM32")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def crc_step(register, byte):
    for bit in range(8):
        feedback = (register ^ (byte >> bit)) & 1
        register >>= 1
        if feedback:
            register ^= 0xA001
    return register


def crc16(data):
    value = 65535
    for byte in data:
        value = crc_step(value, byte)
    return value


def payload(size, pattern=0):
    state, result = 0xC0B50000 ^ size, bytearray()
    for _ in range(size):
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        result.append(0 if pattern == 1 else state & 255)
    return bytes(result)


def encode(data):
    out, code_pos, code, last_full = bytearray([0]), 0, 1, False
    for byte in data:
        if byte:
            out.append(byte)
            code += 1
            if code == 255:
                out[code_pos] = code
                code_pos, code = len(out), 1
                out.append(0)
                last_full = True
        else:
            out[code_pos] = code
            code_pos, code = len(out), 1
            out.append(0)
            last_full = False
    if code == 1 and last_full:
        out.pop()
    else:
        out[code_pos] = code
    return bytes(out) + b"\0"


def wire(body, protocol, policy, wide):
    assert protocol in (0, 1) and policy in range(4)
    protected = body if protocol == 0 else b"\x11\x41" + len(body).to_bytes(2, "big") + body
    if policy:
        protected += crc16(protected).to_bytes(2, "little")
    if protocol:
        return protected
    return encode(len(protected).to_bytes(2 if wide else 1, "little") + protected)


def unwire(frame, protocol, policy, wide):
    if protocol == 0:
        assert frame and frame[-1] == 0 and 0 not in frame[:-1], "COBS delimiter"
        raw, pos, end = bytearray(), 0, len(frame) - 1
        while pos < end:
            count = frame[pos]
            pos += 1
            assert pos + count - 1 <= end, "truncated COBS block"
            raw.extend(frame[pos:pos + count - 1])
            pos += count - 1
            if count < 255 and pos < end:
                raw.append(0)
        width = 2 if wide else 1
        assert len(raw) >= width and int.from_bytes(raw[:width], "little") == len(raw) - width, "COBS length"
        protected = bytes(raw[width:])
    else:
        assert frame[:2] == b"\x11\x41" and len(frame) >= 4, "RTU envelope"
        protected = frame
    if policy:
        assert len(protected) >= 2 and crc16(protected[:-2]) == int.from_bytes(protected[-2:], "little"), "CRC mismatch"
        protected = protected[:-2]
    if protocol:
        assert int.from_bytes(protected[2:4], "big") == len(protected) - 4, "RTU length"
        protected = protected[4:]
    return protected


@lru_cache(maxsize=1)
def hardware_digest():
    # Incremental independent oracle, including all zero-length prefixes.
    data, result = payload(4104), 2166136261
    for offset in range(8):
        register = 65535
        for size in range(1025):
            if size:
                register = crc_step(register, data[offset + size - 1])
            for byte in register.to_bytes(2, "little"):
                result = ((result ^ byte) * 16777619) & 0xFFFFFFFF
    return result


def corpus(case):
    sizes = {"short8": (8,), "long250": (250,), "mixed": (8, 32, 128, 250)}[case]
    return [bytes((i * 37 + n + variant * 19) & 255 for i in range(n))
            if variant % 2 == 0 else bytes(n) for variant in range(10) for n in sizes]


def telemetry(frame, protocol, policy):
    body = unwire(frame, protocol, policy, False)
    assert body[:5] == MAGIC + b"S" and len(body) == 77, "telemetry envelope"
    return struct.unpack("<18I", body[5:])


def check_uart(row, protocol, policy):
    memory = row["memory"]
    assert memory in (0, 1) and row["case"] in ("short8", "long250", "mixed")
    assert row["repetition"] in (0, 1)
    snapshots = []
    for name in ("before", "after"):
        snap = row[name]
        assert bytes.fromhex(snap["tx"]) == wire(MAGIC + b"S", protocol, policy, False)
        words = telemetry(bytes.fromhex(snap["rx"]), protocol, policy)
        assert tuple(snap["words"]) == words, "summary is not raw telemetry"
        assert words[:5] == (2, protocol, policy, memory, 600000000)
        assert words[8] == 0 and words[14:18] == (0, 0, 0, 1), "MCU/UART error"
        snapshots.append(words)
    before, after = snapshots
    count = row["received"]
    assert count == row["sent"] and count > 0 and after[6] - before[6] == count, "lost/extra echo"
    bodies = corpus(row["case"])
    frames = [wire(b, protocol, policy, False) for b in bodies]
    observed = hashlib.sha256()
    byte_count = 0
    for i in range(count):
        observed.update(frames[i % len(frames)])
        byte_count += len(bodies[i % len(bodies)])
    assert observed.hexdigest() == row["observed_wire_sha256"], "wire digest"
    assert after[7] - before[7] == row["payload_bytes"] == byte_count, "payload count"
    seconds = (after[5] - before[5]) / 1000
    assert 1 <= row["requested_seconds"] <= 10
    assert row["requested_seconds"] <= row["host_seconds"] < row["requested_seconds"] + 1
    assert abs(seconds - row["host_seconds"]) < 0.25, "MCU/host time disagreement"
    service = (after[9] + (after[10] << 32)) - (before[9] + (before[10] << 32))
    irq = (after[11] + (after[12] << 32)) - (before[11] + (before[12] << 32))
    assert seconds > 0 and service > 0 and irq > 0 and after[13] > before[13]
    cpu = 100 * (service + irq) / (before[4] * seconds)
    assert 0 < cpu < 100
    return dict(cpu=cpu, cycles_per_echo=(service + irq) / count,
                payload_bytes_per_second=byte_count / seconds, mcu_seconds=seconds)


def check_lines(image):
    p, c = image["protocol"], image["policy"]
    grouped = {}
    for line in image["lines"]:
        fields = line.split(",")
        assert fields[0] in ("HELLO", "VERIFY", "C", "ENDCRC", "F", "R", "ENDCORE"), line
        grouped.setdefault(fields[0], []).append(fields[1:])
    def one(name):
        assert len(grouped.get(name, [])) == 1, name
        return tuple(map(int, grouped[name][0]))
    hello = one("HELLO")
    assert len(hello) == 13 and hello[:5] == (2, p, c, 600000000, 300000000), "clock/selector"
    assert (hello[5] >> 4) & 0xFFF == 0xC27 and hello[6] & (3 << 16) == 3 << 16 and hello[7] & 1
    assert 0x24000000 <= hello[8] < hello[8] + hello[9] <= 0x24071C00 and hello[9] == 131072
    assert 0x24000000 <= hello[10] < hello[10] + hello[11] <= 0x24071C00 and 0 < hello[12] < hello[11]
    assert one("VERIFY") == ((8200, 0, hardware_digest(), 65535) if c == 3 else (0, 0, 0, 0)), "hardware equivalence"
    raw = [tuple(map(int, r)) for r in grouped.get("C", [])]
    expected_raw = Counter((n, o, s) for n in CRC_SIZES for o in (0, 1) for s in range(9)) if c else Counter()
    assert Counter(r[:3] for r in raw) == expected_raw, "raw CRC plan"
    data = payload(4104)
    for r in raw:
        assert len(r) == 7
        n, offset, sample, iterations, cycles, expected, ok = r
        assert iterations == (1 if n >= 1024 else 16) and 0 < cycles < 600000
        assert expected == crc16(data[offset:offset + n]) and ok == 1, "raw CRC"
    assert one("ENDCRC") == (len(raw), 0)
    frames = {}
    for r in grouped.get("F", []):
        assert len(r) == 6
        wide, n, pattern, scenario, size = map(int, r[:5])
        key = wide, n, pattern, scenario
        assert key not in frames
        frame = bytes.fromhex(r[5])
        assert len(frame) == size and frame == wire(payload(n, pattern), p, c, wide), "wire oracle"
        frames[key] = size
    groups = [(w, n, pat, scen) for w, n in SIZES for pat in (0, 1) for scen in range(3)]
    assert set(frames) == set(groups), "missing/extra frame cases"
    rows = [tuple(map(int, r)) for r in grouped.get("R", [])]
    assert Counter(r[:6] for r in rows) == Counter((m, *g, s) for m in (0, 1) for g in groups for s in range(9)), "core plan"
    for r in rows:
        assert len(r) == 15
        memory, wide, n, pattern, scenario, sample, iterations, rx, tx, release, window, size, before, after, ok = r
        assert iterations == (1 if wide else 4) and min(rx, tx, release) > 0
        assert rx + tx + release <= window < 600000
        assert size == frames[wide, n, pattern, scenario] and before == after and ok == 1, "core correctness/leak"
    end = one("ENDCORE")
    assert len(end) == 3 and end[:2] == (648, 0) and 0 < end[2] <= hello[9]
    return dict(hello=hello, raw=raw, rows=rows, heap_committed=end[2])


def summarize(image):
    checked = check_lines(image)
    result = dict(protocol=image["protocol"], policy=image["policy"], heap_committed=checked["heap_committed"], core=[], crc=[], uart=[])
    def stats(values):
        return dict(median=statistics.median(values), minimum=min(values), maximum=max(values))
    for memory in (0, 1):
        for wide, size in SIZES:
            for pattern in (0, 1):
                for scenario in range(3):
                    selected = [r for r in checked["rows"] if r[:5] == (memory, wide, size, pattern, scenario)]
                    result["core"].append(dict(memory=memory, wide=wide, size=size, pattern=pattern, scenario=scenario,
                        total=stats([(r[7] + r[8] + r[9]) / r[6] for r in selected]),
                        rx=stats([r[7] / r[6] for r in selected]), tx=stats([r[8] / r[6] for r in selected]),
                        release=stats([r[9] / r[6] for r in selected])))
    if image["policy"]:
        for size in CRC_SIZES:
            for offset in (0, 1):
                result["crc"].append(dict(size=size, offset=offset,
                    cycles=stats([r[4] / r[3] for r in checked["raw"] if r[:2] == (size, offset)])))
    for r in image["uart"]:
        result["uart"].append(dict(memory=r["memory"], case=r["case"], repetition=r["repetition"], **check_uart(r, image["protocol"], image["policy"])))
    return result


def verify_session(directory, local_images=False):
    receipt = json.loads((directory / "session.json").read_text())
    assert receipt["schema"] == 1 and receipt["completed"] and "error" not in receipt
    assert receipt["restored_and_verified"] and receipt["backup_bytes"] == 65536
    assert receipt["backup_sha256"] == receipt["readback_sha256"]
    plan = [(0, 3)] if receipt["probe"] else [(p, c) for c in (3, 0, 1, 2) for p in (0, 1)]
    assert [tuple(x) for x in receipt["plan"]] == plan
    assert [(i["protocol"], i["policy"]) for i in receipt["images"]] == plan, "image matrix incomplete"
    provenance = Provenance(REPO)
    summaries = []
    submodule_checked = set()
    for image in receipt["images"]:
        p, c = image["protocol"], image["policy"]
        assert image["tag"] == f"{p}-{c}" and image["binary_bytes"] <= 65536
        expected = [] if receipt["probe"] else [(m, case, rep) for m in (0, 1) for case in ("short8", "long250", "mixed") for rep in range(2)]
        assert [(r["memory"], r["case"], r["repetition"]) for r in image["uart"]] == expected, "UART plan"
        assert all(r["requested_seconds"] == receipt["seconds"] for r in image["uart"])
        ordinary = {}
        for relative, sha in image["source_sha256"].items():
            if relative.startswith(("libs/delegate/", "libs/spsc/")):
                if (relative, sha) in submodule_checked:
                    continue
                parts = relative.split("/", 2)
                root = "/".join(parts[:2])
                revision = subprocess.check_output(["git", "rev-parse", f"{receipt['source_base_commit']}:{root}"], cwd=REPO, text=True).strip()
                blob = subprocess.check_output(["git", "show", f"{revision}:{parts[2]}"], cwd=REPO / root)
                assert sha in {hashlib.sha256(b).hexdigest() for b in (blob, blob.replace(b"\r\n", b"\n"), blob.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))}
                submodule_checked.add((relative, sha))
            else:
                ordinary[relative] = sha
        provenance.check(receipt["source_base_commit"], ordinary)
        summaries.append(summarize(image))
        if local_images:
            session = Path(receipt["session"])
            for suffix in ("elf", "bin", "map", "dis", "nm"):
                assert digest(session / (image["tag"] + "." + suffix)) == image["artifact_sha256"][suffix]
            assert image["artifact_sha256"]["elf"] == image["elf_sha256"] and image["artifact_sha256"]["bin"] == image["binary_sha256"]
            symbols = (session / (image["tag"] + ".nm")).read_text()
            tables = [line for line in symbols.splitlines() if "lookup_" in line]
            assert len(tables) == (1 if c == 2 else 0), "unexpected CRC table footprint"
            if tables:
                assert re.match(r"080[0-9a-f]+ 00000200 [rR] ", tables[0]), "table is not exactly 512 read-only bytes"
            for name in ("malloc", "free", "_malloc_r", "_free_r", "_sbrk"):
                assert re.search(rf" [tT] {name}$", symbols, re.MULTILINE), f"missing real allocator symbol {name}"
            if image["uart"]:
                log = session / (image["tag"] + "-transfers.jsonl")
                assert digest(log) == image["transfers_sha256"]
                transfers = iter(json.loads(line) for line in log.read_text().splitlines())
                for row in image["uart"]:
                    frames = [wire(b, p, c, False).hex() for b in corpus(row["case"])]
                    for index in range(row["sent"]):
                        got = next(transfers)
                        assert got == dict(memory=row["memory"], case=row["case"], repetition=row["repetition"], index=index,
                                           tx=frames[index % len(frames)], rx=frames[index % len(frames)]), "raw transfer mismatch"
                assert next(transfers, None) is None, "extra transfers"
    if local_images:
        session = Path(receipt["session"])
        assert digest(session / "before.bin") == digest(session / "after.bin") == receipt["backup_sha256"]
    provenance.report()
    return summaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--local-images", action="store_true")
    parser.add_argument("--summary", type=Path, help="write derived, reproducible metrics (not raw evidence)")
    args = parser.parse_args()
    if not __debug__: parser.error("Python -O is forbidden")
    summaries = verify_session(args.directory, args.local_images)
    if args.summary:
        args.summary.write_text(json.dumps(summaries, indent=2, sort_keys=True) + "\n")
    print("protocol / method / 250-byte random known-size: Pool cycles -> Heap cycles (paired medians)")
    for s in summaries:
        rows = [r for r in s["core"] if (r["size"], r["pattern"], r["scenario"]) == (250, 0, 0)]
        pool, heap = [r["total"]["median"] for r in rows]
        print(f"{'RTU' if s['protocol'] else 'COBS':4} {METHODS[s['policy']]:7} {pool:9.1f} -> {heap:9.1f} ({heap / pool:.3f}x)")
    print(f"PASS {len(summaries)} images; raw CRC, exact wires, Heap/Pool lifecycle, UART matrix and restoration")
    return 0


if __name__ == "__main__":
    sys.exit(main())
