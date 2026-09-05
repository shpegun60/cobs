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
no free fixed-width `store/load` functions.

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
wire byte order. For a reflected algorithm, pass the reflected polynomial.

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

## Policy contract

A protocol accepts any type satisfying `crc::Policy`:

```cpp
struct Policy {
    using value_type = /* any equality-comparable result type */;
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

For integer results, derive from `crc::Codec<Value, WireSize, WireOrder>` and
implement only `calculate()`:

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

Empty built-ins cost no object storage when held with `[[no_unique_address]]`.
A stateful policy costs only its declared state. There are no virtual calls,
function pointers, global mutable state, runtime method branches, allocations,
or exceptions in the API.

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

The host suite checks the four named models against their standard check
values and 20,000 random inputs per width against independent bit-level
oracles. It also covers both wire orders, a three-byte/truncated codec,
stateful custom calculation, corruption, and `NoCrc` under ASan+UBSan and
`-O3 -DNDEBUG`.

The ARM guard compiles 24 Bitwise and 24 Table algorithm objects (four widths,
three optimization levels, and both CPU byte orders). Every calculation loop
is helper-call-free; CRC8/16/32/64 Table emits exactly one private read-only
lookup of 256/512/1024/2048 bytes only when selected. Twelve additional
default/strict-alignment codec matrices prove that every 1/2/4/8-byte
little/big-endian store/load path is call-free and branch-free. A
translation unit that names all table types but executes Bitwise or `NoCrc`
emits zero lookup symbols; the complete `NoCrc` verify path folds to a constant
`true` with no memory access, call, or conditional branch.

Modbus RTU is currently the only consumer. COBS remains independent for now;
the CRC policy module was deliberately extracted so a later COBS integrity
format can use it without depending on Modbus.

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
