#!/usr/bin/env python3
# Author: shpegun60
# SPDX-License-Identifier: MIT

"""
PC driver for the H7S UART bench (uart_bench.cpp over the ST-LINK VCP).

Protocol: single-byte commands sent alone after a line pause so the IDLE
event delivers them as their own chunk. The 48-byte payload pattern excludes
all six current command values: 'R', 'S', 'T', 't', '1', and '3'.

Scenarios:
  0  idle            nothing on the wire, just a window
  1  rx-cont         continuous RX at full line rate
  2  rx-burst        historical 96-byte bursts with a pause (IDLE-heavy)
  3  tx-cont         board floods 64-byte frames, PC drains (clean TX baseline)
  4  duplex-cont     TX generator + continuous RX
  5  duplex-burst    TX generator + bursty RX

Usage:
  python bench.py COM5 --scenario 1 --seconds 10
  python bench.py COM5 --all --seconds 10 --csv results.csv
"""

import argparse
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial required: pip install pyserial")

CMD_PAUSE = 0.15          # line silence around a command byte -> its own chunk
COMMANDS = frozenset(b"RSTt13")
# Keep the historical 48-byte pattern (1,536 bytes per continuous write and
# 96 bytes per burst), but exclude every current command value explicitly.
# A future command addition therefore has one obvious companion edit here.
PAYLOAD = bytes(b for b in range(0x20, 0x80) if b not in COMMANDS)[:48]

# Live speed change (setBaudRate on the board): command byte -> new rate.
SWITCH = {115200: b"1", 3000000: b"3"}

SCENARIOS = {
    0: "idle",
    1: "rx-cont",
    2: "rx-burst",
    3: "tx-cont",
    4: "duplex-cont",
    5: "duplex-burst",
}


def drain(port, max_s=0.0):
    """Read and discard whatever the board is sending (TX generator frames).
    Bounded by wall time: with the generator saturating the line there is no
    'silence' to wait for, an unbounded loop would never return."""
    n = 0
    deadline = time.time() + max_s
    while True:
        chunk = port.read(4096)
        if chunk:
            n += len(chunk)
        if not chunk or time.time() >= deadline:
            return n


def command(port, byte):
    port.flush()            # block until the OS has SENT everything queued,
    time.sleep(CMD_PAUSE)   # then give the line real silence: the command
    port.write(byte)        # must arrive as its own 1-byte IDLE chunk
    port.flush()
    time.sleep(CMD_PAUSE)


def read_report(port, timeout_s=5.0):
    """Collect bytes until a line starting with 'RX=' arrives (generator
    frames are 0x55 bytes with no newline, so lines are unambiguous)."""
    deadline = time.time() + timeout_s
    buf = b""
    while time.time() < deadline:
        buf += port.read(4096)
        for line in buf.splitlines():
            if line.startswith(b"RX="):
                return line.decode(errors="replace")
    raise TimeoutError("no report from the board (got %d bytes)" % len(buf))


def run_scenario(port, num, seconds):
    tx_on = num in (3, 4, 5)
    rx_kind = {1: "cont", 2: "burst", 4: "cont", 5: "burst"}.get(num)

    command(port, b"t")   # force a known state: a crashed previous run may
    drain(port, max_s=1.0)  # have left the generator flooding the line
    command(port, b"R")
    if tx_on:
        command(port, b"T")

    t_end = time.time() + seconds
    if rx_kind == "cont":
        while time.time() < t_end:
            port.write(PAYLOAD * 32)  # 1.5K per blocking write: line-rate RX
            if tx_on:
                drain(port, max_s=0.02)
    elif rx_kind == "burst":
        while time.time() < t_end:
            port.write(PAYLOAD * 2)       # historical 96-byte burst
            port.flush()
            time.sleep(0.02)              # ~20 ms of line silence -> IDLE
            if tx_on:
                drain(port, max_s=0.02)
    else:
        while time.time() < t_end:
            time.sleep(0.05)
            drain(port, max_s=0.05)

    if tx_on:
        command(port, b"t")           # flushes our queue, then stops the flood
        time.sleep(0.3)
        drain(port, max_s=1.0)        # generator tail
    time.sleep(0.3)  # let the last RX chunk flush before the stop command
    drain(port, max_s=0.5)

    command(port, b"S")
    return read_report(port)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port", help="COM port of the ST-LINK VCP, e.g. COM5")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--scenario", type=int, choices=sorted(SCENARIOS))
    ap.add_argument("--all", action="store_true", help="run scenarios 0..5 in order")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--csv", help="append raw report lines to this file")
    ap.add_argument("--switch-to", type=int, choices=sorted(SWITCH),
                    help="tell the board to setBaudRate() to this rate, reopen "
                         "the port there, and verify the link still works")
    args = ap.parse_args()

    if args.switch_to:
        with serial.Serial(args.port, args.baud, timeout=0.05) as port:
            command(port, SWITCH[args.switch_to])
        time.sleep(0.5)  # the board tears down and re-arms reception
        with serial.Serial(args.port, args.switch_to, timeout=0.05) as port:
            print(f"--- after setBaudRate({args.switch_to}) ---")
            print(run_scenario(port, 1, 3.0))
        return

    todo = sorted(SCENARIOS) if args.all else [args.scenario]
    if todo == [None]:
        ap.error("pick --scenario N or --all")

    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        for num in todo:
            name = SCENARIOS[num]
            print(f"--- scenario {num} ({name}), {args.seconds:.0f}s ---")
            report = run_scenario(port, num, args.seconds)
            print(report)
            if args.csv:
                with open(args.csv, "a", encoding="utf-8") as f:
                    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
                    f.write(f"{stamp},{args.baud},{num},{name},{report}\n")


if __name__ == "__main__":
    main()
