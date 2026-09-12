<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# wire: shared bytes, storage and application vocabulary

<!-- toc -->

Contents

- [Normal use](#normal-use)
- [Custom storage](#custom-storage)
- [What not to depend on](#what-not-to-depend-on)

<!-- /toc -->

[Documentation](../../doc/README.md) · [API parity](../../doc/API_PARITY.md) · [Examples](../../doc/EXAMPLES.md)

## Normal use

The protocol public headers expose their needed wire types automatically.
Select `wire::Heap` or `wire::Pool<RxOwners, TxOwners>` as Endpoint's first
argument. No protocol-specific allocator facade is needed.

All three protocols share `wire::SendResult` and the same `read_native`,
`read_be`, `read_le`, `read_bytes`. The caller owns the cursor; failed reads
leave it and the output unchanged. read_bytes returns a borrowed span;
scalar readers read one value, and writers additionally accept typed spans.
See [the complete reading/writing example](../../doc/START_HERE_UK.md#читання-та-запис-полів).

## Custom storage

Implement `Memory::For<Geometry>` with four nonthrowing acquire/release methods.
Geometry supplies alignment-rounded maximum RX bytes, maximum TX bytes and
RX alignment. Protocol headers, fields, queues, CRC and refcounts remain
outside your allocator. The full [storage contract and implementation example](../../doc/STORAGE.md)
includes overgrants, exact descriptor return, alignment and conformance checks.

## What not to depend on

Do not construct protocol RxBlock internals or mix Packet owners across
endpoint lifetimes. Do not interpret shared storage policy as a shared global
allocator instance: each endpoint owns its own bound Storage unless a custom
policy explicitly refers to an external arena. [Tests](../../doc/TESTING.md)
lock this common vocabulary without merging protocol implementations.
