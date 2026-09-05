<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# CRC policy library

`crc/Crc.h` is a protocol-independent, header-only C++20 library. It separates
the check-value algorithm from its wire representation so a protocol can use a
CRC8, CRC16, CRC32, CRC64, hardware engine, private checksum, or no trailer at
all without assuming a result type or byte count.

## Built-in policies

```cpp
#include "crc/Crc.h"

crc::Crc8Bitwise   crc8;
crc::Crc8Table     fast_crc8;
crc::Crc16Bitwise  crc16;      // default CRC-16/MODBUS model
crc::Crc16Table    fast_crc16;
crc::Crc32Bitwise  crc32;      // default CRC-32/ISO-HDLC model
crc::Crc32Table    fast_crc32;
crc::Crc64Bitwise  crc64;      // default CRC-64/ECMA-182 model
crc::Crc64Table    fast_crc64;
crc::NoCrc         no_crc;
```

Policies are ordinary objects:

```cpp
const uint16_t value = crc16.calculate(bytes);

std::array<uint8_t, crc::Crc16Bitwise::wire_size> trailer{};
crc16.store(trailer.data(), value);
const uint16_t decoded = crc16.load(trailer.data());

const bool valid = crc::verify<crc::Crc16Bitwise>(complete_frame);
```

The free `calculate/verify` helpers are generic orchestration only. Calculation
state and all wire serialization remain owned by the policy class; there are
no free fixed-width `store/load` functions. The overloads that default-construct
the policy are unconditionally `noexcept` and therefore accept only policies
whose default constructor cannot throw; any other policy is constructed by the
caller and passed by reference.

Every `Bitwise` policy is table-free. A `Table` policy owns one private static
256-entry lookup specialized for its exact width and parameters. Including the
header, declaring a table-policy alias, or using only a bitwise policy emits no
lookup object. A lookup is emitted in read-only program memory only when that
exact table implementation is called; it never occupies policy-object RAM.
There is no namespace-scope/global lookup variable. The Cortex-M object guard
checks this at `-Os`, `-O2`, and `-O3` for every supported width.

The defaults are named standard models and pass their `"123456789"` check
values:

| Type | Default model | Check value | Wire order |
|---|---|---:|---|
| `Crc8` | CRC-8/SMBUS | `0xF4` | big-endian (one byte) |
| `Crc16` | CRC-16/MODBUS | `0x4B37` | little-endian |
| `Crc32` | CRC-32/ISO-HDLC | `0xCBF43926` | little-endian |
| `Crc64` | CRC-64/ECMA-182 | `0x6C40DF5F0B497347` | big-endian |

`Crc8`, `Crc16`, `Crc32`, and `Crc64` are configurable alias templates. Their
parameters are method, polynomial, initial value, final XOR, reflection, and
wire byte order.

```cpp
using FastModbus = crc::Crc16<crc::Table>;

using PrivateCrc32 = crc::Crc32<
    crc::Bitwise,
    0x82F63B78u,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    true,
    std::endian::little>;
```

The parameters are the **register values of the implementation**, not the
catalogue (Rocksoft) model values. For a non-reflected model they are the
same. For a reflected model the engine keeps its register bit-reversed, so
both the polynomial **and the initial value** must be reflected, while the
final XOR is applied after reflection and stays as catalogued:

| Catalogue parameter | Reflected model | Non-reflected model |
|---|---|---|
| `poly` | `reflect(poly)` | `poly` |
| `init` | `reflect(init)` | `init` |
| `xorout` | `xorout` | `xorout` |

The four defaults all have bit-symmetric initial values, so the rule only
bites on models such as CRC-16/ISO-IEC-14443-3-A (catalogue init `0xC6C6`,
register `0x6363`): only the reflected value reproduces its check `0xBF05`;
the verbatim value computes `0x1480`. The host suite locks both. Two model
families are deliberately not expressible: `refin != refout`, and widths other
than 8/16/32/64 (CRC-24, CRC-15/CAN, CRC-7). A CRC-24 polynomial passed to
`Crc32` compiles and computes something that is not CRC-24.

The register type is constrained by `crc::Value` to exactly `uint8_t`,
`uint16_t`, `uint32_t` and `uint64_t`. `bool` and distinct character types
such as `char16_t` are rejected. Typedefs retain type identity: `unsigned char`,
`unsigned long` or `size_t` are accepted where they alias an allowed type.
This is a type boundary, not a check of how the caller spelled the type.

## Policy contract

A protocol accepts any type satisfying `crc::Policy`:

```cpp
struct Policy {
    using value_type = /* equality-comparable result, nothrow conversion to bool */;
    static constexpr std::size_t wire_size = /* compile-time byte count */;

    value_type calculate(std::span<const uint8_t>) noexcept;
    void store(uint8_t*, value_type) noexcept;
    value_type load(const uint8_t*) noexcept;
};
```

The contract is structural, not semantic. The library does not inspect the
polynomial, compare against a built-in, or require the result to be a CRC. A
peer may intentionally use a wrapping sum as long as both ends select the same
policy and wire codec.

For fixed-width unsigned results, derive from
`crc::Codec<Value, WireSize, WireOrder>` and implement only `calculate()`:

```cpp
struct Sum32 : crc::Codec<uint32_t, 4, std::endian::little> {
    uint32_t calculate(std::span<const uint8_t> bytes) noexcept
    {
        uint32_t value = 0;
        for (uint8_t byte : bytes) {
            value += byte;
        }
        return value;
    }
};
```

`Codec` stores and loads explicitly byte by byte. It never copies the native
object representation and therefore does not depend on host endianness or
alignment. `WireSize` may be smaller than `sizeof(Value)` for an intentionally
truncated field; in that case `calculate()` must return the same truncated
value that `load()` reconstructs, or the policy must provide its own codec.
`NoCrc` uses a zero-byte codec: calculation, store, and load all fold away, and
no trailer is read or written.

Stateful hardware policies are ordinary objects:

```cpp
struct HardwareCrc32 : crc::Codec<uint32_t, 4, std::endian::little> {
    explicit HardwareCrc32(CRC_HandleTypeDef& peripheral) noexcept
        : handle(&peripheral) {}

    CRC_HandleTypeDef* handle;

    uint32_t calculate(std::span<const uint8_t> bytes) noexcept
    {
        return calculate_with_peripheral(*handle, bytes);
    }
};
```

Calculation is synchronous: its input is borrowed only until return and may
have byte alignment (not necessarily peripheral-word alignment). A hardware
adapter must handle the span's alignment/placement, complete any hardware
operation before returning, and keep its peripheral handle valid. It must not
retain the input for asynchronous DMA after the call.

Empty built-ins are eligible for `[[no_unique_address]]`; actual compression
is an ABI/compiler choice. A stateful object adds its state and any alignment
padding. See the [measured layouts](../doc/SHARED_POLICIES_VALIDATION.md).
Built-in calculations use no virtual calls, function pointers, mutable global
state, runtime algorithm dispatch, allocations or exceptions. A custom policy
is responsible for its own implementation and nonthrowing contract.

## Build and verification

The module consists of one public header. Add the repository root to the
compiler include path, or use the qmake fragment:

```qmake
include(path/to/crc/crc.pri)
```

Run the host and Cortex-M checks with:

```bash
sh crc/tests/run.sh
sh crc/tests/check_arm_codegen.sh
python -B crc/tests/check_arm_matrix.py
```

The host suite checks the four default models plus CRC-16/CCITT-FALSE,
CRC-16/GENIBUS, CRC-32/BZIP2, CRC-32/ISCSI, CRC-8/MAXIM-DOW, CRC-64/XZ and
CRC-16/ISO-IEC-14443-3-A against their catalogue check values, at compile time
and at run time, in both Bitwise and Table form. The defaults are additionally
compared with bit-level oracles on 20,000 random inputs per width. It also
covers both wire orders, a three-byte/truncated codec, stateful custom
calculation, corruption, `NoCrc`, the `crc::Value` boundary and the refusal
of a throwing default constructor by the default-constructing helpers. The
`-O1` build runs under ASan+UBSan only where the toolchain provides the
runtime (WSL g++ does, MinGW does not); `run.sh` prints which build it made.
The `-O3 -DNDEBUG` build runs everywhere.

The ARM guard compiles 24 Bitwise and 24 Table algorithm objects (four widths,
three optimization levels, and both CPU byte orders). Every calculation loop
is helper-call-free; CRC8/16/32/64 Table emits exactly one private read-only
lookup of 256/512/1024/2048 bytes only when selected. Twelve additional
default/strict-alignment codec matrices prove that every 1/2/4/8-byte
little/big-endian store/load path is call-free and branch-free. A
translation unit that names all table types but executes Bitwise or `NoCrc`
emits zero lookup symbols; the complete `NoCrc` verify path folds to a constant
`true` with no memory access, call, or conditional branch.

Both COBS and Modbus RTU use these policies without depending on each other:
`cobs::Endpoint<Memory, cobs::Format<Crc>>` and
`modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Crc, MaxAdu>>`.
Both default to CRC16 Bitwise; a custom stateful policy is injected as
`Link{MyCrc{handle}}` and the same object serves RX and TX. See the shared
[storage contract](../doc/STORAGE.md) and [COBS wire format](../doc/PROTOCOL.md).

For a wider CPU check, `check_arm_matrix.py` queries the installed GNU Arm
compiler's full CPU list and builds every named target: all nine policies,
Os/O2/O3, little/big-endian, and explicit strict-alignment codec probes.
It inspects the actual object disassembly and read-only table symbols.
The scope is AArch32 (M-profile/STAR-MC1 in Thumb, other targets in ARM state),
soft-float, no LTO; it does not claim AArch64 or execution on those CPUs.
See the [ARM audit report](tests/ARM_AUDIT.md).

Real Cortex-M7 results for all nine policies, including pure calculation
cycles, integrated CPU, exact flashed-image disassembly and table sizes, are
in the [NUCLEO-H7S3L8 benchmark](../modbus/rtu/tests/hardware/h7s/CRC_BENCHMARK.md).
