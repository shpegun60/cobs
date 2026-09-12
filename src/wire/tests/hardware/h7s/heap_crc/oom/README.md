<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Live out-of-memory diagnosis, 2026-09-12

**The suspected nano-runtime behavior is reproduced on NUCLEO-H7S3L8.**
Seven allocation paths reach the real `abort()` and `_exit(1)` instead of
returning a nullable allocation failure. This is a successful reproduction
of a defect/limitation, **not** a claim that production OOM handling passes.
No production allocator, protocol or global C++ allocation operator was fixed
or replaced by this diagnostic.

**Subsequent fix:** [`wire::Heap` recovery tests](../recovery/README.md) now
pass with malloc/free. This directory remains the immutable pre-fix
diagnostic and intentionally expects the old Heap's abort behavior. Its
recorded source version, not the current header, defines those results.
Use the recovery harness for the current library; do not rerun this old
expected-abort suite against the fixed Heap and interpret that mismatch as
a new production failure.

## Results

Source: [raw UART observations and source/image identities](results_2026-09-12/session.json).

| Command | Operation after exhausting the heap | Observed result |
|---|---|---|
| `M` | `malloc(64)` | `nullptr`; after freeing held blocks, Heap TX/RX recover |
| `P` | Pool-backed COBS and RTU message construction and RX | Both work while Heap is exhausted; Heap TX/RX recover after freeing |
| `N` | `operator new(64, std::nothrow)` | `abort` -> real `_exit(1)`; no return to the command loop |
| `C` | COBS Heap `make_message(250)` | `abort` -> real `_exit(1)` |
| `R` | RTU Heap `make_message(0x11, 3, 250)` | `abort` -> real `_exit(1)` |
| `c` | COBS Heap RX of a valid frame | `abort` -> real `_exit(1)` |
| `r` | RTU Heap RX of a valid complete ADU | `abort` -> real `_exit(1)` |
| `G` | Grow an existing COBS Heap message from capacity 1 | `abort` -> real `_exit(1)` |
| `g` | Grow an existing RTU Heap message from capacity 1 | `abort` -> real `_exit(1)` |

One image was flashed, with a fresh hardware reset between cases. A 128 KiB
AXI SRAM arena backed real newlib `malloc/free`; the existing test-only
[`_sbrk` backing](../heap.cpp) was reused unchanged. There were 52 live held
allocations totaling 130,642 payload bytes, or 51 totaling 130,641 bytes
when a message was already alive for the growth test. The arena committed
all 131,072 bytes. All four allocation-size stages reached real malloc
failure, and a final `malloc(1)` also failed: this is actual exhaustion,
not an allocator stub forced to return null.

The successful controls also prove that the valid COBS and RTU vectors are
accepted, Pool does not depend on Heap allocation here, and ordinary Heap
operation resumes after releasing the held memory. The default `new_handler`
was checked to be null; no recovery handler was installed.

## Why the evidence identifies abort, not just a timeout

The image links `--wrap=abort --wrap=_exit` **only for observation**. Each
observer prints its entry over the polling UART, without printf/stdio or
heap allocation, then unconditionally calls the original function. Neither
observer returns, substitutes an error, frees memory nor changes allocation
behavior. The real linked path is:

```text
failed malloc in operator new
  -> get_new_handler() == nullptr
  -> abort observer -> real abort -> raise(SIGABRT)
  -> _exit observer -> real generated Cube _exit infinite loop
```

The [linked allocator/observer disassembly](results_2026-09-12/allocator_path.dis)
is retained with its hash. Verification checks that both observers forward
to the real functions, that real abort reaches the exit observer, and that
the original `_exit` retains its terminal loop. For each fatal case the raw
UART sequence is `EXHAUSTED`, `BEFORE`, `ABORT`, `EXIT`; a subsequent `H`
request receives nothing. Silence alone cannot pass the verifier. For both
controls the subsequent `H` response proves the command loop remains alive.

This is a GNU Arm 14.3.1 `libstdc++_nano`, `-Os`, `-fno-exceptions`, no-LTO
result on this board/build. It is not a claim that every implementation of
`new(std::nothrow)` behaves this way. The portable storage requests nullable
allocation, but this linked runtime does not deliver it on the observed
failure path. A production correction remains separate work.

## Reproduce and verify

```powershell
& 'C:/Program Files/Git/bin/bash.exe' src/wire/tests/hardware/h7s/heap_crc/oom/build.sh
python -B src/wire/tests/hardware/h7s/heap_crc/oom/test_verify.py
python -B src/wire/tests/hardware/h7s/heap_crc/oom/run.py `
  --port COM6 --serial 002A001F3033510135393935 `
  --output path/to/a-new-oom-result
python -B src/wire/tests/hardware/h7s/heap_crc/oom/verify.py `
  src/wire/tests/hardware/h7s/heap_crc/oom/results_2026-09-12 --local-images
```

The runner uses exclusive board access, never retries a runtime case, refuses
existing output directories, and backs up all 65,536 boot-flash bytes before
writing. Original firmware was restored and read back after this run:
`a5903024dba85fab5121150ca8ad13482f97384aa450aab67413881991fb9456`.
The diagnostic image is 21,132 bytes; ELF SHA-256:
`21be9725426aa18c05413a2c6a3052c58120051a0d4cd7c60c38b88f6ca105ba`.

The receipt and allocator disassembly are portable evidence. Local ignored
session files additionally retain the complete ELF/bin/map/dis/nm, programmer
logs and backup/read-back images; `--local-images` authenticates those too.
Five host tests include rejection of missing abort/exit observations,
insufficient exhaustion, incorrect exit status, falsely responsive fatal
cases, hanging controls and non-forwarding observer disassembly.

See also the [Heap/CRC performance measurements](../../../../../../../doc/HEAP_AND_HARDWARE_CRC.md).
