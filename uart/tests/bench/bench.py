#!/usr/bin/env python3
"""
PC driver for the H7S UART bench (uart_bench.cpp over the ST-LINK VCP).

Protocol: single-byte commands sent alone after a line pause so the IDLE
event delivers them as their own chunk. Payload avoids the four command
bytes 'R' 'S' 'T' 't' (the pattern uses 0x20..0x4F, which excludes all four).

Scenarios:
  0  idle            nothing on the wire, just a window
  1  rx-cont         continuous RX at full line rate
  2  rx-burst        100-byte bursts with a pause (IDLE-heavy)
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
PAYLOAD = bytes(range(0x20, 0x50))  # excludes 'R'(0x52) 'S'(0x53) 'T'(0x54) 't'(0x74)

SCENARIOS = {
    0: "idle",
    1: "rx-cont",
    2: "rx-burst",
    3: "tx-cont",
    4: "duplex-cont",
    5: "duplex-burst",
}


def drain(port):
    """Read and discard whatever the board is sending (TX generator frames)."""
    n = 0
    while True:
        chunk = port.read(4096)
        if not chunk:
            return n
        n += len(chunk)


def command(port, byte):
    time.sleep(CMD_PAUSE)
    port.write(byte)
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

    drain(port)
    command(port, b"R")
    if tx_on:
        command(port, b"T")

    t_end = time.time() + seconds
    if rx_kind == "cont":
        while time.time() < t_end:
            port.write(PAYLOAD * 8)   # 384 bytes per write, back to back
            drain(port)
    elif rx_kind == "burst":
        while time.time() < t_end:
            port.write(PAYLOAD[:50] * 2)  # a 100-byte burst
            port.flush()
            time.sleep(0.02)              # ~20 ms of line silence -> IDLE
            drain(port)
    else:
        while time.time() < t_end:
            time.sleep(0.05)
            drain(port)

    if tx_on:
        command(port, b"t")
        time.sleep(0.3)
        drain(port)
    time.sleep(0.3)  # let the last RX chunk flush before the stop command
    drain(port)

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
    args = ap.parse_args()

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
                    f.write(f"{stamp},{num},{name},{report}\n")


if __name__ == "__main__":
    main()
