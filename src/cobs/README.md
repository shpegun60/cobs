<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# COBS module

<!-- toc -->

Contents

- [Application entry](#application-entry)
- [Complete guides](#complete-guides)

<!-- /toc -->

[Documentation](../../doc/README.md) · [Почни звідси](../../doc/START_HERE_UK.md) · [Examples](../../doc/EXAMPLES.md)

## Application entry

Include `cobs/Cobs.h` with the repository `src` and `libs/delegate` include
roots; compile Encoder.cpp and Decoder.cpp once. Start with:

```cpp
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
// Heap: cobs::Endpoint<>. Larger useful payload:
using Large = cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>;
```

Default useful data is 253 bytes with CRC16. Format chooses the envelope;
Pool counts owners, not physical bytes. COBS accepts arbitrarily cut byte
streams through consume and publishes immutable Packet handles. It has no
RTU-style incomplete-frame timer. Sender/BusyQuery or an adapter connect it
to a transport; Sent transfers local ownership, not a peer acknowledgement.

## Complete guides

- [COBS public user guide and API table](../../doc/USER_GUIDE.md#cobs-quick-start).
- [Qt recipes](../../doc/QT.md), [FreeRTOS and raw/manual binding](../../doc/FREERTOS.md).
- [Exact wire protocol and CRC compatibility](../../doc/PROTOCOL.md).
- [Shared storage](../../doc/STORAGE.md), [CRC policy guide](../crc/README.md).
- [Architecture](../../doc/ARCHITECTURE.md), [testing/evidence](../../doc/TESTING.md).

`detail/` is internal; `tests/` is verification, not a runtime dependency.
