<!-- Author: shpegun60
SPDX-License-Identifier: MIT -->

# Paired COBS / Modbus RTU hardware benchmark

See [methodology, results and limitations](../../../../doc/PROTOCOL_COMPARISON.md).

- `protocol_bench.cpp` / `build.sh`: endpoint-only live DWT measurements;
  UART is the untimed result channel, not part of these cycle counts.
- `run_comparison.py`: backups/restores the board, builds and verifies exact
  images, checks independent wire oracles, runs matched UART traffic and
  preserves separate high-baud framing probes.
- `test_comparison.py`: four offline oracle/corpus/cadence tests.
- `verify_comparison.py`: independently checks coverage, source identities,
  counters, formulas and optionally retained images / published table rows.
- [Recorded 5 September session](results_comparison_2026-09-05.json): 225 core
  configurations, 2,025 timing windows, 168 paired UART windows and six probes.

The probe section includes RTU failures at 3M/6M/10M on this VCP/IDLE adapter;
it is not a claim that all high-baud physical exchanges passed. No production
protocol or UART code is altered by the benchmark.
