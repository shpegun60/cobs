<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Runnable examples

<!-- toc -->

Contents

- [Run from the repository root](#run-from-the-repository-root)
- [What is application code and what is test scaffolding](#what-is-application-code-and-what-is-test-scaffolding)

<!-- /toc -->

[Full catalog and explanations](../EXAMPLES.md) · [Qt](../QT.md) · [FreeRTOS](../FREERTOS.md) · [Start here](../START_HERE_UK.md)

## Run from the repository root

```sh
python -B doc/check_docs.py
sh doc/examples/build.sh
sh doc/examples/qt/build.sh
```

`build.sh` runs 16 portable/host-fake configurations. `qt/build.sh` runs three
Qt programs; serial self-tests use a fake port, TCP uses localhost. Neither
flashes a board. `check_freertos_arm.sh` optionally compiles both task-entry
variants against real H7RS HAL/FreeRTOS headers, without executing firmware.

## What is application code and what is test scaffolding

`freertos_entry.cpp` exposes the real task entry outside `DOC_HOST`; choose
RTU with `DOC_RTU=1`. Other MCU programs have explicit host mains that inject
fake interrupts. `platform_fake.h`, `qt/LoopPort.h` and `Example.h` are
test/example helpers, not extra library dependencies for your firmware.

`host_entry.cpp` includes the fake peripheral model before the selected example
in one translation unit. This guarantees that static UART destruction still
has a live fake HAL; the builder does not rely on cross-TU linker order or
skip destructors to make sanitizer runs pass.

See [the catalog](../EXAMPLES.md) for each filename, supported mode, expected
behavior and the documented lifecycle/error policy. Generated binaries and
logs go under ignored `out/` directories.
