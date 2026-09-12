<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Documentation index

<!-- toc -->

Contents

- [Start and use](#start-and-use)
- [Current contracts and module reference](#current-contracts-and-module-reference)
- [Tests, measurements and hardware receipts](#tests-measurements-and-hardware-receipts)
  - [Latest complete checkpoint](#latest-complete-checkpoint)
  - [Earlier checkpoints and investigations](#earlier-checkpoints-and-investigations)
  - [Performance and reproducible harnesses](#performance-and-reproducible-harnesses)
- [Design history, not starting templates](#design-history-not-starting-templates)
- [Maintaining this documentation](#maintaining-this-documentation)

<!-- /toc -->

[Repository](../README.md) · [Почни звідси](START_HERE_UK.md) · [Examples](EXAMPLES.md)

This is the entry point, not another architecture specification. The first
group is for using the libraries today. Dated reports describe exactly what
was tested at that checkpoint; plans and old code are history, not API recipes.

## Start and use

| Document | Read it when… |
|---|---|
| [Почни звідси — українською](START_HERE_UK.md) | you have forgotten the project, its files or policies |
| [User guide](USER_GUIDE.md) | you need the complete COBS/UART public walkthrough and API tables |
| [Example catalog](EXAMPLES.md) | you want a runnable source file for a specific task |
| [Integration](INTEGRATION.md) | you are choosing a transport and need lifecycle/buffering contracts |
| [Qt](QT.md) | you need QSerialPort COBS, an RTU client/server or QTcpSocket MBAP |
| [FreeRTOS](FREERTOS.md) | you need a sleeping task with the same COBS/RTU service loop |
| [Build](BUILD.md) | you need include paths, sources, qmake/CMake or toolchain commands |
| [Testing](TESTING.md) | you need the right check and its actual coverage boundary |

## Current contracts and module reference

Module landing pages: [COBS](../src/cobs/README.md), [RTU](../src/modbus/README.md),
[TCP](../src/modbus/tcp/README.md), [UART](../src/uart/README.md),
[wire](../src/wire/README.md), [CRC](../src/crc/README.md), [adapters](../src/adapters/README.md).

| Document | Owns this subject |
|---|---|
| [Shared API parity](API_PARITY.md) | common names and intentional COBS/RTU/TCP differences |
| [Payload limits](PAYLOAD_LIMITS.md) | useful bytes versus automatically added envelope bytes |
| [Architecture](ARCHITECTURE.md) | layers, ownership and execution domains |
| [COBS wire protocol](PROTOCOL.md) | normative encoding, integrity and peer compatibility |
| [Storage](STORAGE.md) | Geometry, Heap, Pool, custom four-operation contract |
| [CRC](../src/crc/README.md) | models, Bitwise/Table, codecs and custom stateful calculators |
| [Modbus RTU](../src/modbus/README.md) | framing, metadata, read/write and recovery |
| [Modbus TCP](../src/modbus/tcp/README.md) | MBAP, connection boundaries, format and ownership |
| [Modbus architecture](../src/modbus/ARCHITECTURE.md) | protocol-specific internal invariants |
| [COBS engine](COBS_ENGINE.md) | detailed encoder/decoder and allocation rationale |
| [Qt client recovery](QT_CLIENT_RECOVERY.md) | retries, late responses and transaction boundaries |

## Tests, measurements and hardware receipts

Begin with [Testing](TESTING.md) to distinguish host fakes, real Qt,
compile/disassembly guards and live MCU evidence. Reports below are snapshots;
a later table does not retroactively change an older measurement.

### Latest complete checkpoint

- [Extension-contract full H7S repeat, 2026-09-12](HARDWARE_EXTENSIONS_2026-09-12.md): 81 image configurations and source/image receipts.
- [Extension contract audit](EXTENSION_CONTRACT_AUDIT.md): reader/storage/CRC boundary fixes behind that repeat.

### Earlier checkpoints and investigations

- [Reader/CRC/FreeRTOS contract hardening](CONTRACT_HARDENING_2026-09-12.md).
- [Cross-stack audit, 2026-09-12](PARANOID_AUDIT_2026-09-12.md).
- [Cross-stack audit, 2026-09-11](PARANOID_AUDIT_2026-09-11.md).
- [Full hardware regression, 2026-09-12](HARDWARE_REGRESSION_2026-09-12.md).
- [API parity and real FreeRTOS hardware follow-up](HARDWARE_API_PARITY_2026-09-12.md).
- [Hardware regression, 2026-09-07](HARDWARE_REGRESSION_2026-09-07.md).
- [Shared-policy validation](SHARED_POLICIES_VALIDATION.md).
- [COBS paranoid audit](COBS_PARANOID_AUDIT.md).
- [UART paranoid audit](UART_PARANOID_AUDIT.md).
- [Qt/USB timeout diagnosis](QT_USB_TIMEOUT_DIAGNOSIS.md).

### Performance and reproducible harnesses

- [COBS CPU and throughput matrix](COBS_PERFORMANCE.md).
- [Matched COBS versus RTU comparison](PROTOCOL_COMPARISON.md).
- [Heap versus Pool and STM32 CRC](HEAP_AND_HARDWARE_CRC.md).
- [CRC width/method H7S benchmark](../src/modbus/rtu/tests/hardware/h7s/CRC_BENCHMARK.md).
- [All GNU AArch32 CRC code-generation audit](../src/crc/tests/ARM_AUDIT.md).
- [COBS H7S harness](../src/cobs/tests/hardware/h7s/README.md).
- [RTU H7S harness](../src/modbus/rtu/tests/hardware/h7s/README.md).
- [TCP MCU/UART harness](../src/modbus/tcp/tests/hardware/h7s/README.md).
- [UART benchmark](../src/uart/tests/bench/README.md).
- [Shared protocol hardware harness](../src/wire/tests/hardware/h7s/README.md).
- [Heap/OOM recovery harness](../src/wire/tests/hardware/h7s/heap_crc/recovery/README.md).

## Design history, not starting templates

- [Legacy code removal record and recovery commands](LEGACY_REVIEW.md).
- [COBS refactor plan](COBS_REFACTOR_PLAN.md).
- [Shared-policy migration plan](SHARED_POLICIES_PLAN.md).
- [Modbus TCP implementation plan](MODBUS_TCP_PLAN.md).
- [Old UART/COBS architecture and UART implementations in Git](https://github.com/shpegun60/cobs/tree/f09494a/doc/old).

Do not copy types or defaults from old plans into a new application. Start
from [Examples](EXAMPLES.md), then use the current contract for the relevant
module. Historical JSON/JSONL/CSV data is preserved, not regenerated by the
documentation tools.

## Maintaining this documentation

The root README is a landing page; this index routes readers; module guides
own technical detail. Runnable examples are the source of truth for complete
programs. A fenced fragment is labelled as a fragment unless linked to its
complete translation unit.

Run `python -B doc/check_docs.py` after changing links or examples. It checks
repository-local Markdown targets and anchors in maintained documentation,
navigation/contents blocks and synchronized example excerpts. Then run
[the appropriate example builders](EXAMPLES.md#build-and-verification).
It does not certify external web links or replace protocol/hardware tests.
