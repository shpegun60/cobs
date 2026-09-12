<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Legacy UART review: doc/old is not a dependency

<!-- toc -->

Contents

- [Inventory and current use](#inventory-and-current-use)
- [Why it confuses users](#why-it-confuses-users)
- [Removal and scope](#removal-and-scope)
- [How to inspect or recover it later](#how-to-inspect-or-recover-it-later)

<!-- /toc -->

[Documentation](README.md) · [Current integration](INTEGRATION.md) · [Archived Git tree](https://github.com/shpegun60/cobs/tree/f09494a/doc/old)

## Inventory and current use

The removed `doc/old` directory contained 23 tracked files,
135,792 bytes: two Markdown documents and 21 legacy source/helper files.
They include the C UART/container variants, STM32UartDMA, UartEngine/UartBase,
an RS485Engine wrapper, IRQGuard and older utility/type headers.

A search of the active `src/`, `app/` and qmake/build definitions found no
include or build dependency on these legacy implementations. References from
the current UART header, CLAUDE.md and earlier audits point to historical
rationale, not compiled code. The last existing change to the archive was
commit `f09494a` (`Document COBS and UART libraries`); Git history retains it.

## Why it confuses users

The old UART README presents obsolete IT/DMA/circular modes, RX work in ISR,
UartBase callback routing, aliases and configuration flags as if they were
current. Its license placeholder and half-transfer discussion are not the
current library contract. The new driver deliberately uses normal-mode DMA,
its own callback integration, ordered gaps, thread-context RX and disabled HT.

The RS-485 DE wrapper and the old IT/circular API are historical features,
not claimed one-for-one equivalents of the current driver. Deleting the old
code would remove those examples from the checkout; it would not disable
a feature of the active library. Do not quietly advertise them as current
capabilities in the new user guide.

## Removal and scope

The user explicitly approved removing the directory in the documentation
commit. All 23 previously tracked files were removed, including the old design
sketch. Current documentation links and the UART's historical comment now
refer to the Git snapshot, not nonexistent local files. No active implementation
was replaced by an archived one; current library behavior is unchanged.

There is no requirement to preserve these checkout copies for building or
testing the current repository. Git retains every removed file. This dependency
check does not speak for unrelated external projects that copied the old API.

## How to inspect or recover it later

Read-only Git commands, if a later cleanup removes the checkout copies:

```sh
git ls-tree -r f09494a -- doc/old
git show f09494a:doc/old/uart_engine/README.md
git log -- doc/old
```

The active starting point is [Почни звідси](START_HERE_UK.md), not this archive.
