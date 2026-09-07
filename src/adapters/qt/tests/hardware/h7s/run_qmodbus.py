"""The comparison against QtSerialBus on the NUCLEO-H7S3L8, both ways round.

For every baud and framing mode asked for, two images of the RTU harness are
built, inspected and flashed:

  * the board as a SERVER (MODBUS_HW_ROLE=1) serving the reference model at
    unit 0x11; the shared script is then run against it twice from the PC —
    by Qt's QModbusRtuSerialClient and by this repository's RtuClient — and
    every scenario's verdict and round-trip time is recorded for both;
  * the board as a CLIENT (MODBUS_HW_ROLE=2): the PC runs Qt's
    QModbusRtuSerialServer serving the same model at unit 0x0A, the board runs
    the same script against it and reports each step's verdict against its
    shadow of the model and the round-trip time; Qt's final register map and
    every write it saw are recorded too.

The board's flash is backed up first and restored, verified and read back
last. One JSON record holds everything with the source hashes it was built
from; verify_qmodbus.py rechecks it and prints the tables for the README.

  python -B src/adapters/qt/tests/hardware/h7s/run_qmodbus.py --port COM6 --serial <st-link> \\
      --output src/adapters/qt/tests/hardware/h7s/results_qmodbus_<date>.json
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path


def repository_root(start: Path) -> Path:
    """The git root: the directory holding COBS.pro, found by walking up from `start`."""
    for candidate in (start, *start.parents):
        if (candidate / "COBS.pro").is_file():
            return candidate
    raise RuntimeError(f"repository root (COBS.pro) not found above {start}")


HERE = Path(__file__).resolve().parent
REPO = repository_root(HERE)   # the git root
SRC = REPO / "src"
RTU_HW = SRC / "modbus/rtu/tests/hardware/h7s"
QT_TESTS = SRC / "adapters/qt/tests"
sys.path.insert(0, str(RTU_HW))
import modbus_hardware as hw  # noqa: E402
import serial  # noqa: E402

ELF = REPO / "stm32_cube_test/h7s_cobs_test/out/modbus-hardware/modbus_hardware_bench.elf"
PROGRAMMERS = sorted(glob.glob(
    "C:/ST/STM32CubeIDE_*/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_*/tools/bin/STM32_Programmer_CLI.exe"))
DEFAULT_BAUDS = (115200, 1000000)

# What the record is built from; verify_qmodbus.py checks these hashes against commits.
SOURCES = [
    "src/modbus/rtu/tests/hardware/h7s/modbus_bench.cpp",
    "src/modbus/rtu/tests/hardware/h7s/build.sh",
    "src/modbus/rtu/tests/hardware/h7s/modbus_hardware.py",
    "src/modbus/rtu/tests/reference_model.h",
    "src/adapters/rtu/UartAdapter.h",
    "src/adapters/qt/SerialAdapter.h",
    "src/adapters/qt/RtuClient.h",
    "src/adapters/qt/qt.pri",
    "src/adapters/qt/tests/qmodbus_bench/main.cpp",
    "src/adapters/qt/tests/qmodbus_bench/ServerTrace.h",
    "src/adapters/qt/tests/qmodbus_bench/qmodbus_bench.pro",
    "src/adapters/qt/tests/hardware/h7s/run_qmodbus.py",
    "src/uart/Uart.h",
]   # the verifier is not among them: it judges the record and may be corrected afterwards


def sha256(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_identities() -> dict:
    identities = {}
    for relative in SOURCES:
        identities[relative] = sha256(REPO / relative)
    for folder in ("modbus/rtu", "modbus/rtu/detail", "wire", "wire/detail", "crc", "modbus"):
        for path in sorted((SRC / folder).glob("*.h")):
            identities[path.relative_to(REPO).as_posix()] = sha256(path)
    return identities


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--serial", required=True, help="ST-LINK serial number")
    parser.add_argument("--output", type=Path, required=True, help="JSON record (must not exist)")
    parser.add_argument("--bauds", default=",".join(map(str, DEFAULT_BAUDS)))
    parser.add_argument("--framers", default="0,1", help="MODBUS_HW_FRAMER values, comma separated")
    parser.add_argument("--policy", default="bitwise")
    parser.add_argument("--timeout-ms", type=int, default=1000, help="response timeout of both PC clients")
    parser.add_argument("--retries", type=int, default=0, help="retries of both PC clients (0: every failure is visible)")
    parser.add_argument("--server-seconds", type=int, default=10, help="how long Qt's server serves the board's script")
    parser.add_argument("--client-delay-ms", type=int, default=2500, help="the board's head start given to Qt's server")
    parser.add_argument("--server-inter-frame-us", type=int, default=50000,
                        help="Qt server RX fragment deadline for USB/OS delivery; -1 keeps Qt's native default")
    parser.add_argument("--server-trace", action=argparse.BooleanOptionalAction, default=True,
                        help="capture Qt server decisions in memory (default on; disable explicitly for untraced timing)")
    parser.add_argument("--stall-first-read-fragment-ms", type=int, default=0,
                        help="test only: one host-side stall after a partial FC03 request; requires --server-trace")
    parser.add_argument("--programmer", default=PROGRAMMERS[-1] if PROGRAMMERS else None)
    parser.add_argument("--bash", default="C:/Program Files/Git/bin/bash.exe")
    parser.add_argument("--qt-kit", default="C:/Qt/6.4.3/mingw_64")
    parser.add_argument("--mingw-bin", default="C:/Qt/Tools/mingw1120_64/bin")
    args = parser.parse_args()
    if not -1 <= args.server_inter_frame_us <= 1000000 or not 0 <= args.stall_first_read_fragment_ms <= 200:
        parser.error("server deadline or diagnostic stall is outside its supported range")
    if args.stall_first_read_fragment_ms and not args.server_trace:
        parser.error("--stall-first-read-fragment-ms requires --server-trace")
    bauds = [int(b) for b in args.bauds.split(",")]
    framers = [int(m) for m in args.framers.split(",")]
    assert all(m in (0, 1) for m in framers) and bauds and framers
    assert args.programmer and Path(args.programmer).exists(), "STM32_Programmer_CLI.exe not found"
    output = args.output if args.output.is_absolute() else REPO / args.output
    assert not output.exists(), f"refusing to overwrite an existing record: {output}"
    policy = hw.CRC_POLICIES[args.policy]

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session = REPO / f"stm32_cube_test/h7s_cobs_test/out/qmodbus-{stamp}"
    session.mkdir(parents=True)
    connection = ["-c", "port=SWD", f"sn={args.serial}", "mode=UR", "reset=HWrst", "freq=4000"]

    def command(tag, argv, env=None, check=True, cwd=REPO):
        with (session / f"{tag}.log").open("w", encoding="utf-8") as log:
            completed = subprocess.run(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT)
        if check and completed.returncode != 0:
            raise RuntimeError(f"{tag} failed with exit code {completed.returncode}; see {session / (tag + '.log')}")
        return completed.returncode

    def flash(tag, path, attempts=3):
        for attempt in range(1, attempts + 1):
            log = f"{tag}-flash" if attempt == 1 else f"{tag}-flash-retry{attempt}"
            command(log, [args.programmer, *connection, "-w", str(path), "0x08000000", "-v", "-rst"], check=False)
            text = (session / (log + ".log")).read_text(encoding="utf-8", errors="replace")
            if "Download verified successfully" in text:
                time.sleep(0.5)
                return attempt
        raise RuntimeError(f"{tag}: flash failed {attempts} times; see {session}")

    @contextmanager
    def link(baud, framer):
        hw.FRAMED = bool(framer)
        hw.FRAME_PREFIX = 2 if framer else 0
        with serial.Serial(args.port, baud, timeout=0.02) as port:
            time.sleep(0.2)
            port.reset_input_buffer()
            yield hw.HardwareLink(port, policy)

    # ---- the PC side is built once, with the Qt kit that has QtSerialBus
    runner_out = QT_TESTS / "out/qmodbus_bench"
    runner_out.mkdir(parents=True, exist_ok=True)
    qt_env = os.environ.copy()
    qt_env["PATH"] = f"{args.qt_kit}/bin;{args.mingw_bin};" + qt_env["PATH"]
    command("runner-qmake", [f"{args.qt_kit}/bin/qmake.exe", str(QT_TESTS / "qmodbus_bench/qmodbus_bench.pro")],
            env=qt_env, cwd=runner_out)
    command("runner-make", [f"{args.mingw_bin}/mingw32-make.exe", "-j"], env=qt_env, cwd=runner_out)
    runner = runner_out / "bin/qmodbus_bench.exe"
    assert runner.exists(), runner

    def run_pc(tag, role, baud, extra=()):
        out = session / f"{tag}-{role}.json"
        argv = [str(runner), "--role", role, "--port", args.port, "--baud", str(baud), "--out", str(out),
                "--timeout", str(args.timeout_ms), "--retries", str(args.retries), *extra]
        command(f"{tag}-{role}", argv, env=qt_env)
        return json.loads(out.read_text(encoding="utf-8"))

    receipt = dict(schema=1, session=str(session), started=stamp, port=args.port, serial=args.serial,
                   policy=args.policy, bauds=bauds, framers=framers, images=[], completed=False,
                   restored_and_verified=False, runner_sha256=sha256(runner))
    record = dict(schema=1, started=stamp, port=args.port, policy=args.policy, bauds=bauds, framers=framers,
                  timeout_ms=args.timeout_ms, retries=args.retries, server_seconds=args.server_seconds,
                  client_delay_ms=args.client_delay_ms,
                  server_inter_frame_us=args.server_inter_frame_us, server_trace=args.server_trace,
                  stall_first_read_fragment_ms=args.stall_first_read_fragment_ms,
                  qt_logging_rules=qt_env.get("QT_LOGGING_RULES", ""),
                  source_base_commit=subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip(),
                  source_sha256=source_identities(), runner_sha256=sha256(runner), runs=[])

    def save():
        (output.parent / (output.name + ".session.json")).write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        output.write_text(json.dumps(record, indent=1, sort_keys=True) + "\n", encoding="utf-8")

    def build_and_flash(tag, role, baud, framer):
        environment = os.environ.copy()
        environment.update(MODBUS_HW_BAUD=str(baud), MODBUS_HW_CRC_POLICY=args.policy, MODBUS_HW_FRAMER=str(framer),
                           MODBUS_HW_ROLE=str(role), MODBUS_HW_OPT="-Os", MODBUS_HW_LTO="0")
        command(tag + "-build", [args.bash, str(RTU_HW / "build.sh")], environment)
        command(tag + "-inspect", [sys.executable, "-B", str(RTU_HW / "inspect_image.py"), str(ELF),
                                   "--policy", args.policy, "--baud", str(baud),
                                   "--optimization", "Os", "--lto", "0", "--framer", str(framer)])
        manifest = json.loads(ELF.with_suffix(".image.json").read_text(encoding="utf-8"))
        shutil.copy(ELF, session / (tag + ".elf"))
        shutil.copy(ELF.with_suffix(".image.json"), session / (tag + ".image.json"))
        attempts = flash(tag, ELF)
        image = dict(tag=tag, role=role, baud=baud, framer=framer, elf_sha256=sha256(ELF),
                     binary_sha256=manifest["binary_sha256"], flash_attempts=attempts)
        receipt["images"].append(image)
        save()
        return image

    backup = session / "before.bin"
    command("backup", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(backup), "-rst"])
    assert backup.stat().st_size == 65536
    receipt["backup_sha256"] = sha256(backup)
    save()
    try:
        for framer in framers:
            for baud in bauds:
                # ---------------- the board serves, two PC clients ask
                tag = f"server-{args.policy}-{baud}-framer{framer}"
                print(f"BUILD + FLASH {tag}", flush=True)
                image = build_and_flash(tag, 1, baud, framer)
                with link(baud, framer) as board:
                    hello = board.hello()
                    before = board.role_report(0)
                    assert before["role"] == 1, before
                    board.reset_model()
                qt_client = run_pc(tag, "qtclient", baud)
                print(f"  QModbusRtuSerialClient: {qt_client['summary']}", flush=True)
                with link(baud, framer) as board:
                    board.reset_model()   # the same initial contents for the second client
                our_client = run_pc(tag, "ourclient", baud)
                print(f"  RtuClient:              {our_client['summary']}", flush=True)
                with link(baud, framer) as board:
                    after = board.role_report(0)
                record["runs"].append(dict(role="server", baud=baud, framer=framer, image=image, hello=hello,
                                           board_counters_before=before["counters"], board_counters_after=after["counters"],
                                           qtclient=qt_client, ourclient=our_client))
                save()

                # ---------------- the board asks, Qt's server answers
                tag = f"client-{args.policy}-{baud}-framer{framer}"
                print(f"BUILD + FLASH {tag}", flush=True)
                image = build_and_flash(tag, 2, baud, framer)
                with link(baud, framer) as board:
                    hello = board.hello()
                    idle = board.role_report(0)
                    assert idle["role"] == 2 and not idle["running"] and not idle["done"], idle
                    board.start_client(args.client_delay_ms)
                server_options = ["--seconds", str(args.server_seconds),
                                  "--server-inter-frame-us", str(args.server_inter_frame_us),
                                  "--stall-first-read-fragment-ms", str(args.stall_first_read_fragment_ms)]
                if args.server_trace:
                    server_options.append("--server-trace")
                qt_server = run_pc(tag, "qtserver", baud, server_options)
                with link(baud, framer) as board:
                    results = board.client_results()
                    board_stats = board.stats()  # after the script: diagnose loss without changing its traffic
                assert results["done"] and not results["running"], results
                statuses = {}
                for entry in results["entries"]:
                    statuses[entry["status"]] = statuses.get(entry["status"], 0) + 1
                print(f"  board client vs QModbusRtuSerialServer: {statuses}; Qt saw {len(qt_server['writes'])} writes", flush=True)
                record["runs"].append(dict(role="client", baud=baud, framer=framer, image=image, hello=hello,
                                           board=results, board_stats=board_stats, qtserver=qt_server))
                save()
        receipt["completed"] = True
    finally:
        restored = False
        for attempt in range(1, 4):
            log = "restore" if attempt == 1 else f"restore-retry{attempt}"
            command(log, [args.programmer, *connection, "-w", str(backup), "0x08000000", "-v", "-rst"], check=False)
            restored = "Download verified successfully" in (session / (log + ".log")).read_text(encoding="utf-8", errors="replace")
            if restored:
                break
        after = session / "after.bin"
        command("readback", [args.programmer, *connection, "-u", "0x08000000", "0x10000", str(after), "-rst"], check=False)
        receipt["restored_and_verified"] = restored and after.exists() and sha256(after) == receipt["backup_sha256"]
        record["restored_and_verified"] = receipt["restored_and_verified"]
        save()
        receipt["results_sha256"] = sha256(output)
        (output.parent / (output.name + ".session.json")).write_text(
            json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"RESTORED {receipt['restored_and_verified']}; record {output}", flush=True)

    # Completing the runner is not equivalent to passing its scenarios. Keep
    # failed records/receipts, restore first, then return the verifier's verdict.
    return subprocess.run([sys.executable, "-B", str(HERE / "verify_qmodbus.py"), str(output)], cwd=REPO).returncode


if __name__ == "__main__":
    sys.exit(main())
