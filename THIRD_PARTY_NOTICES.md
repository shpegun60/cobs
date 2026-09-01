<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Third-party notices

The root [MIT license](LICENSE) covers the project-owned COBS, UART, tests,
documentation, and integration code. It does not replace the licenses of the
Git submodules or vendor-derived files listed below.

| Component | Repository path | License source |
|---|---|---|
| `tiny::delegate` | `libs/delegate` | [`libs/delegate/LICENSE`](libs/delegate/LICENSE) |
| SPSC containers | `libs/spsc` | [`libs/spsc/LICENSE`](libs/spsc/LICENSE) |
| STM32F1 HAL driver | `libs/stm32f1xx-hal-driver` | [`libs/stm32f1xx-hal-driver/LICENSE.md`](libs/stm32f1xx-hal-driver/LICENSE.md) |
| STM32F1 CMSIS device package | `libs/cmsis-device-f1` | [`libs/cmsis-device-f1/License.md`](libs/cmsis-device-f1/License.md) |
| STM32G4 HAL driver | `libs/stm32g4xx-hal-driver` | [`libs/stm32g4xx-hal-driver/LICENSE.md`](libs/stm32g4xx-hal-driver/LICENSE.md) |
| STM32G4 CMSIS device package | `libs/cmsis-device-g4` | [`libs/cmsis-device-g4/License.md`](libs/cmsis-device-g4/License.md) |

The three portability-test configuration files below are derived from STM32
HAL templates and retain their embedded STMicroelectronics copyright and
license notices:

- `uart/tests/port/f1/stm32f1xx_hal_conf.h`;
- `uart/tests/port/g4/stm32g4xx_hal_conf.h`;
- `uart/tests/port/h7rs/stm32h7rsxx_hal_conf.h`.

Raw `.csv` and `.jsonl` benchmark/audit outputs intentionally contain no
comment header so they remain directly machine-readable and preserve their
recorded evidence bytes.
