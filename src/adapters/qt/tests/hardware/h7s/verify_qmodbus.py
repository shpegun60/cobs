"""Rechecks a run_qmodbus.py record and prints the README tables.

What "ok" means: the peer answered exactly what modbus/rtu/tests/reference_model.h
predicts for that request, or stayed silent where the model says nobody
answers. The deliberate exceptions, all following from how length-driven
framing and QtSerialBus behave, are spelled out in EXPECTED below and are the
only non-"ok" verdicts this script accepts:

  * an unknown function (script step 19) has no entry in a framing table, so
    a framed server — the board's with MODBUS_HW_FRAMER=1, and Qt's, which
    frames the same way — never sees a complete request and stays silent
    instead of answering exception 01; only the burst server, which takes a
    whole IDLE-ended burst as one candidate, answers 01. A client sends the
    unknown function as it is (the builder has no opinion about a function
    its table does not know; the peer decides), so the board's client times
    out against Qt's server in both framing modes.
  * Qt's client reports a Diagnostics response as InvalidResponseError at
    its API because it models no data unit for function 0x08; the runner
    judges the raw response, which is exactly the echo the model predicts,
    and keeps Qt's verdict in the scenario's detail.

Everything else must be "ok", the board's flash must have been restored and
read back byte for byte, and every source hash must be a committed version.

  python -B src/adapters/qt/tests/hardware/h7s/verify_qmodbus.py <record.json> [--check-doc README.md]
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path


def repository_root(start: Path) -> Path:
    for candidate in (start, *start.parents):
        if (candidate / "COBS.pro").is_file():
            return candidate
    raise RuntimeError(f"repository root (COBS.pro) not found above {start}")


HERE = Path(__file__).resolve().parent
REPO = repository_root(HERE)
SRC = REPO / "src"
sys.path.insert(0, str(SRC / "wire/tests"))
from provenance import Provenance  # noqa: E402

STEPS = 55
UNKNOWN_FUNCTION_STEP = 19
SCRIPTED_STEPS = 25


def expected_pc_status(step: int, framer: int) -> set[str]:
    """Verdicts a PC client may record against the board's server."""
    if step == UNKNOWN_FUNCTION_STEP and framer == 1:
        return {"timeout"}   # the framed board server cannot frame a function it does not know
    return {"ok"}


def expected_board_status(step: int, framer: int) -> set[str]:
    """Verdicts the board's client may record against Qt's server."""
    del framer
    if step == UNKNOWN_FUNCTION_STEP:
        # the request goes out; Qt's server, which frames by function, ignores
        # it instead of answering exception 01
        return {"timeout"}
    return {"ok"}


def check_server_trace(record, run, require):
    """New diagnostic/configuration fields are absent in historical records."""
    if "server_inter_frame_us" not in record:
        return
    requested = record["server_inter_frame_us"]
    # Qt 6.4.3 rounds to milliseconds and never goes below its baud minimum.
    baud = run["baud"]
    minimum_ms = 2 if baud >= 19200 else (38500 + baud - 1) // baud
    expected_us = max(minimum_ms, (requested + 999) // 1000) * 1000
    server = run["qtserver"]
    require(server.get("inter_frame_delay_us") == expected_us, "Qt server fragment deadline disagrees with the selected configuration")
    trace = server.get("trace", {})
    require(trace.get("enabled") == record["server_trace"], "Qt server trace enable flag disagrees with the record")
    require(trace.get("overflow") is False, "Qt server trace overflowed or is absent")
    stall = record["stall_first_read_fragment_ms"]
    require(trace.get("stall_requested_ms") == stall, "diagnostic stall request was relabelled")
    require(trace.get("stall_injected") == bool(stall), "requested diagnostic stall was not injected exactly once")
    if stall:
        require(trace.get("stall_elapsed_us", 0) >= stall * 1000, "diagnostic host stall was shorter than requested")
        require(bool(trace.get("stall_fragment")), "diagnostic stall has no observed partial request")
    else:
        require(trace.get("stall_elapsed_us") == 0 and trace.get("stall_fragment") == "", "unexpected injected stall")
    entries = trace.get("entries", [])
    require(bool(entries) == record["server_trace"], "enabled Qt server trace is empty, or disabled trace contains events")
    times = [entry["at_us"] for entry in entries]
    require(times == sorted(times) and all(t >= 0 for t in times), "Qt server trace timestamps are not monotonic")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("record", type=Path)
    parser.add_argument("--check-doc", type=Path, help="README whose table rows must match the record")
    args = parser.parse_args()
    record = json.loads(args.record.read_text(encoding="utf-8"))
    failures = []

    def require(condition, message):
        if not condition:
            failures.append(message)

    require(record.get("restored_and_verified") is True, "the board's flash was not restored and read back")
    require(record.get("retries") == 0, "clients must run without retries so every failure is visible")
    expected = {(role, baud, framer) for role in ("server", "client")
                for baud in record["bauds"] for framer in record["framers"]}
    keys = [(run["role"], run["baud"], run["framer"]) for run in record["runs"]]
    require(len(keys) == len(expected) and set(keys) == expected, "missing or duplicate board role/baud/framer run")
    receipt = json.loads(Path(str(args.record) + ".session.json").read_text(encoding="utf-8"))
    require(receipt.get("completed") and receipt.get("restored_and_verified"), "session did not complete and restore")
    require(receipt.get("results_sha256") == hashlib.sha256(args.record.read_bytes()).hexdigest(), "record bytes disagree with the session receipt")
    require([run["image"] for run in record["runs"]] == receipt["images"], "run image identities disagree with the flashed-image receipt")

    # ---- provenance
    # check() raises on a hash no committed version at or after the base has;
    # report() prints what matched where, CAVEAT lines being the acceptable
    # weakenings (sources still uncommitted, verified against the working tree).
    provenance = Provenance(REPO)
    try:
        provenance.check(record["source_base_commit"], record["source_sha256"])
    except AssertionError as error:
        failures.append(str(error))
    provenance.report()

    rows_server = []
    rows_client = []
    for run in record["runs"]:
        baud, framer = run["baud"], run["framer"]
        require(run["image"]["role"] == (1 if run["role"] == "server" else 2), f"{run['role']} run at {baud}: wrong image role")
        require(run["hello"]["framer"] == framer, f"{run['role']} run at {baud}: HELLO framer disagrees with the image")
        require(run["image"]["baud"] == baud and run["image"]["framer"] == framer and run["hello"]["baud"] == baud,
                f"{run['role']} run at {baud}: baud/framer metadata mismatch")
        if run["role"] == "server":
            cells = []
            for client in ("qtclient", "ourclient"):
                scenarios = run[client]["scenarios"]
                require(len(scenarios) == STEPS, f"{client} at {baud}/framer{framer}: {len(scenarios)} scenarios, expected {STEPS}")
                bad = [s for s in scenarios if s["status"] not in expected_pc_status(s["index"], framer)]
                for s in bad:
                    failures.append(f"{client} at {baud}/framer{framer}: step {s['index']} '{s['name']}' "
                                    f"{s['status']} {s.get('detail', '')} received={s.get('received', '')}")
                ok = sum(1 for s in scenarios if s["status"] == "ok")
                summary_ = run[client]["summary"]
                cells.append((ok, summary_.get("burst_median_us"), summary_.get("total_us")))
            served = run["board_counters_after"]["served"] - run["board_counters_before"]["served"]
            rows_server.append((baud, framer, cells, served))
        else:
            check_server_trace(record, run, require)
            entries = run["board"]["entries"]
            require(len(entries) == STEPS, f"board client at {baud}/framer{framer}: {len(entries)} steps reported")
            bad = [e for e in entries if e["status"] not in expected_board_status(e["index"], framer)]
            for e in bad:
                failures.append(f"board client at {baud}/framer{framer}: step {e['index']} {e['status']} "
                                f"detail={e['detail']} responded={e['responded']}")
            ok = sum(1 for e in entries if e["status"] == "ok")
            burst = sorted(e["rtt_us"] for e in entries if e["index"] >= SCRIPTED_STEPS and e["status"] == "ok")
            median = burst[len(burst) // 2] if burst else None
            writes = run["qtserver"]["writes"]
            final_holding = run["qtserver"]["final_holding"]
            final_coils = run["qtserver"]["final_coils"]
            # the writes the script performs, as Qt's map must show them afterwards
            require(final_holding[5] == 0xBEEF, f"Qt server at {baud}/framer{framer}: register 5 is not 0xBEEF after the script")
            require(final_holding[6] == 0x1234, f"Qt server at {baud}/framer{framer}: the broadcast write to register 6 did not land")
            require(final_holding[10] == 0xA000 and final_holding[19] == 0xA009,
                    f"Qt server at {baud}/framer{framer}: the 10-register write did not land")
            require(final_holding[20] == 0x7000 and final_holding[142] == 0x707A,
                    f"Qt server at {baud}/framer{framer}: the 123-register write did not land")
            # step 12 wrote 100..103, then step 15's 123-register write (20..142) overwrote them
            require(final_holding[100] == 0x7050 and final_holding[103] == 0x7053,
                    f"Qt server at {baud}/framer{framer}: the read/write-multiple and 123-register writes did not land in order")
            require(final_coils[7] == 1 and final_coils[16] == 1 and final_coils[17] == 0,
                    f"Qt server at {baud}/framer{framer}: the coil writes did not land")
            require(len(run["qtserver"]["errors"]) == 0, f"Qt server at {baud}/framer{framer} reported errors: {run['qtserver']['errors']}")
            rows_client.append((baud, framer, ok, median, len(writes), [e for e in entries if e["status"] != "ok"]))

    for line in failures:
        print("FAIL", line)
    if not failures:
        print(f"PASS {len(record['runs'])} runs; flash restored and read back; every verdict as the reference model predicts")

    def fmt_us(value):
        return "n/a" if value is None else f"{value / 1000:.2f} ms"

    lines = ["", "### The board as a server: Qt's client and this repository's client, same script, same server", "",
             "| Baud | Board server | QModbusRtuSerialClient ok | RtuClient ok | Qt burst median | RtuClient burst median | Qt total | RtuClient total | served by the board |",
             "|---:|---|---:|---:|---:|---:|---:|---:|---:|"]
    for baud, framer, cells, served in rows_server:
        (qt_ok, qt_med, qt_total), (our_ok, our_med, our_total) = cells
        lines.append(f"| {baud} | {'framing policy' if framer else 'burst'} | {qt_ok}/{STEPS} | {our_ok}/{STEPS} | "
                     f"{fmt_us(qt_med)} | {fmt_us(our_med)} | {fmt_us(qt_total)} | {fmt_us(our_total)} | {served} |")
    lines += ["", "### The board as a client against QModbusRtuSerialServer", "",
              "| Baud | Board client | steps ok | burst median round trip | writes Qt recorded | steps not ok |",
              "|---:|---|---:|---:|---:|---|"]
    for baud, framer, ok, median, writes, bad in rows_client:
        bad_text = "; ".join(f"{e['index']} {e['status']}" for e in bad) or "—"
        lines.append(f"| {baud} | {'framing policy' if framer else 'burst'} | {ok}/{STEPS} | {fmt_us(median)} | {writes} | {bad_text} |")
    table = "\n".join(lines)
    print(table)

    if args.check_doc:
        doc = args.check_doc.read_text(encoding="utf-8")
        missing = [line for line in lines if line.startswith("| ") and not line.startswith("| Baud") and line not in doc]
        if missing:
            print(f"FAIL {len(missing)} table rows are not in {args.check_doc}:")
            for line in missing:
                print("  ", line)
            failures.extend(missing)
        else:
            print(f"PASS {sum(1 for l in lines if l.startswith('| ') and not l.startswith('| Baud'))} table rows present in {args.check_doc}")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
