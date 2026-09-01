# UART paranoid correctness and hot-path audit

Status: automated audit, implementation, and fresh H7S3 silicon validation
complete; ready for a source-control checkpoint

Audit date: 2026-09-01

Audited baseline: `9f1ba179a66f42982715d27d3fdee98f7794412c`

Scope: every active production file under `uart/`, the fake-HAL runtime suite,
the Cortex-M portability matrix, the H7S hardware benchmark harness, and the
relevant STM32 F1, G4, and H7RS HAL/CMSIS implementation paths. Historical
drivers under `doc/old/` were used only as rationale, never as active code.

This document is the canonical record of the audit. The header remains the
executable API contract. The existing COBS architecture remains independent:
UART owns byte movement and loss reporting; COBS owns framing and packet
lifetime.

## 1. Outcome

The architecture is retained and hardened rather than replaced:

- C++20 and the typed `Uart<ChunkSize, ChunkCount>` facade remain;
- owning `tiny::delegate` handlers remain;
- normal-mode DMA remains the only RX/TX transport mode;
- RX remains zero-copy into an inline SPSC chunk pool;
- TX remains a single borrowed caller span with no internal queue;
- the STM32 HAL backend remains; no speculative LL rewrite was introduced;
- internal weak callbacks, external forwarding, and registered HAL callbacks
  remain supported;
- there is still no virtual dispatch, dynamic registry allocation, mutex, or
  mandatory heap use.

The audit did find material correctness and hot-path work:

1. The common RX callback no longer calls
   `HAL_UARTEx_GetRxEventType()`. `RxState == READY` identifies the normal
   IDLE/TC completion first; event-kind lookup exists only in the anomalous
   still-running branch.
2. Every successful RX and TX DMA start explicitly disables the half-transfer
   interrupt. This must happen on every re-arm because the HAL enables HT
   again on every `HAL_*_DMA` start.
3. A corrupt DMA remaining count is rejected before unsigned subtraction or
   D-cache maintenance can escape the active chunk.
4. Blocking abort timeouts, including the H7RS GPDMA suspend-timeout state,
   preserve ownership and repair only a direction whose peripheral DMA
   request is already disabled.
5. Baud changes now restore FIFO configuration transactionally and reapply
   the previous baud plus saved FIFO hardware state if either the new UART
   init or FIFO restoration fails.
6. Frequently touched state was moved into a 64-byte ARM control header ahead
   of the inline DMA buffers. Hot ISR accesses now use short offsets.
7. Re-entrant `proceed()` calls from handlers are harmless no-ops instead of
   observing and popping the same front chunk twice.
8. Configuration limits, registry entries, DMA instances, runtime framing,
   watchdog states, and stats snapshots now fail or behave explicitly.

## 2. Locked design decisions

The following are intentional constraints, not unfinished refactoring:

| Decision | Reason |
|---|---|
| C++20 template facade | compile-time chunk geometry, typed spans, no runtime polymorphism |
| `tiny::delegate` handlers | preserves owned captures and explicit `borrow`/`bind`; heap fallback is off by default |
| DMA normal mode only | ownership transfers at one terminal IDLE/TC boundary; circular/linked-list mode would keep writing published storage |
| RX SPSC chunks inline in `Uart` | one DMA producer, one loop consumer, no allocation or copy on receive |
| one borrowed TX span | the COBS layer already owns retry/queue policy; duplicating a queue here would split ownership |
| HAL backend | historical H7S silicon measurements showed too little CPU cost to justify an LL fork and its per-family maintenance burden |
| fixed-capacity registry | HAL callbacks provide a handle but no portable user context across the supported families |
| plain diagnostic counters | exact accounting is not worth atomic RMW operations in the ISR hot paths |
| callbacks not globally deferred | RX/gap already run in the loop; TX/error preserve immediate HAL terminal notification |

Removing delegates, translating the driver to C, adding compatibility wrappers,
or introducing allocator traits are outside this architecture.

## 3. Ownership and lifetime model

| Owner | State or memory | Transfer/release boundary |
|---|---|---|
| application | `UART_HandleTypeDef`, RX/TX DMA handles, peripheral setup, NVIC, clock, TX byte span | handles outlive `Uart`; TX span remains valid until `tx_busy()` becomes false / `TxHandler` fires |
| `uart::detail::Registry` | at most `UART_ENGINE_MAX_INSTANCES` handle-to-object entries and three operation pointers | attach after complete validation; detach before destruction/abort |
| `Uart` | control state, stats, SPSC pool, drop buffer, handler delegates | object is neither copyable nor movable; static lifetime is strongly recommended |
| RX DMA | either one claimed unpublished chunk or `m_drop` | ownership ends only after HAL has stopped the receive transfer |
| RX ISR | SPSC producer state and publication | commit length, publish, clear `m_active`, then re-arm |
| main loop | published front chunk and RX/gap handlers | span is valid only during `RxHandler`; `pop()` returns the slot to the producer |
| TX DMA | caller-owned byte span | exactly one terminal success/failure event releases the borrow |
| `tiny::delegate` | ordinary callable captures in inline storage; explicit non-owning target for `borrow`/`bind` | replacement/destruction in thread context; never replace the callable while it is executing |

The central RX invariant is:

```text
free slot -> claimed in m_active -> DMA-owned -> stopped ->
commit + publish -> consumer-owned -> pop -> free slot
```

No recovery path may publish or discard `m_active` while DMA can still write
it. No TX recovery path may clear `m_txBusy` while DMA can still read the
borrowed caller buffer.

### 3.1 Physical field groups

The field order is deliberate:

```text
Uart
├── 64-byte control header on ARM
│   ├── m_huart, m_active
│   ├── watchdog timestamps/counters
│   ├── started/busy/gap/work/service flags
│   ├── per-direction teardown gates
│   └── Stats
├── RxFifo m_rx                 inline DMA-accessible slots
├── alignas(32) m_drop          overflow DMA target
└── four tiny::delegate values  RX, TX, error, gap
```

`m_active` is a volatile pointer because an ISR can re-arm RX while thread
code is inside a blocking abort with interrupts enabled. Within serialized
ISR/`IrqGuard` code it is snapshotted once, avoiding repeated volatile loads.
The payload slots remain cache-line aligned and precede their metadata, which
is required for safe M7 invalidation.

## 4. Execution contexts and callback contract

| Entry | Context | Contract |
|---|---|---|
| `init`, setters, `setBaudRate`, `proceed`, `send` | one application thread / main-loop domain | no concurrent calls; `send` has one producer context |
| `RxHandler` | inside `proceed()` | supplied span dies when the callback returns |
| `GapHandler` | inside `proceed()` | ordered between bytes before and after physical loss |
| `ErrorHandler` | HAL error ISR | bounded, non-blocking, non-throwing |
| `TxHandler` | normally completion/error ISR; watchdog terminal recovery can call it from `proceed()` | must be valid in both contexts and produce exactly one ownership event |
| registry forwarding | HAL ISR | fixed-table lookup followed by non-virtual operation dispatch |

Handlers must not throw. A handler must not replace the same delegate while
that delegate is executing. ISR-side TX/error handlers signal the normal loop
and do not call mutating driver APIs. RX/gap handlers already run in that loop
and may call `send()` under its single-producer rule. Recursive `proceed()` is
explicitly ignored, but this does not make concurrent calls from two threads
valid.

## 5. Initialization and configuration proof

`init()` is one-shot and transactional. Before the object publishes itself in
the registry it validates:

- non-null UART/peripheral/RX-DMA/TX-DMA hardware instances;
- UART TX/RX and both DMA handle states are `READY`;
- both DMA `Parent` pointers refer back to the same UART;
- RX and TX are different handles and different hardware channels;
- peripheral/memory directions and increment modes are exact;
- classic circular mode and GPDMA linked-list mode are absent;
- baud is non-zero;
- parity is an exact supported value, and the transport carries exactly eight
  payload bits: 8B without parity or 9B with even/odd parity;
- UART mode is `TX_RX`, not half-duplex;
- flow control is one exact NONE/RTS/CTS/RTS+CTS mode;
- all source/destination widths are bytes.

Registry attachment rejects null operations, duplicate targets or handles,
two handles aliasing the same peripheral instance, table exhaustion, and null
callback dispatch. A failed first RX arm removes the registry entry and
cancels the unpublished SPSC claim, so retrying `init()` neither leaks a slot
nor invents a stream gap.

All `UART_ENGINE_*` configuration macros must have the same values in every
translation unit that includes the header. `UART_ENGINE_IMPLEMENT` is defined
in exactly one translation unit unless registered callbacks or explicit
external forwarding are used.

## 6. RX hot path and half-transfer behavior

### 6.1 Normal IDLE/TC event

```text
HAL callback
  -> registry lookup
  -> reject stopped/teardown state
  -> RxState == READY          normal fast branch
  -> DMB
  -> read frozen DMA counter
  -> validate remaining <= ChunkSize
  -> commit/publish active slot
  -> claim/reuse next slot
  -> invalidate future DMA target if D-cache is active
  -> HAL_UARTEx_ReceiveToIdle_DMA
  -> disable DMA_IT_HT again
```

The HAL-supplied `size` is not authoritative. HAL samples it before every byte
in the IRQ-entry-to-abort window is necessarily frozen; the stopped DMA count
is read later and includes those bytes. A counter greater than `ChunkSize` is
treated as corruption and produces an ordered gap without touching memory
outside the slot.

### 6.2 Why HT code still exists

HT delivery is disabled in normal operation, but the HAL reenables its DMA
interrupt bit at every RX/TX start. Therefore the driver disables HT after
every successful start, not just once during initialization.

The fake HAL reenables the HT bit on every start and asserts that the driver
clears it after the initial RX arm, RX re-arm, and TX start.

The remaining `HAL_UART_RXEVENT_HT` recognition is a cold safety net for an
interrupt that was already pending or for an unexpected HAL/family behavior.
It returns without publishing or re-arming because the DMA still owns the
buffer. In the old-HAL fallback build, any callback with RX still running is
likewise ignored. The normal READY branch performs zero event-type queries;
the fake HAL has a dedicated assertion for this.

### 6.3 Overflow and gap ordering

When no chunk is free, DMA is armed onto `m_drop`, `rx_overrun` increases, and
the producer stops publishing later data until `proceed()` announces the gap.
Queued chunks therefore all precede the gap. Only after notification does the
loop abort the drop transfer and reclaim a real slot. Failed aborts preserve
ownership and retry instead of exposing a live DMA buffer.

## 7. TX and recovery correctness

`send()` rejects empty, unbound, busy, and greater-than-65,535-byte spans. It
cleans the exact borrowed span for D-cache, resets progress observation, starts
DMA under a short IRQ guard, disables TX HT, and publishes `m_txBusy` in the
same serialized transition as the hardware start.

TX completion checks both UART state and DMA error state before returning the
borrow. The periodic watchdog handles two different cases:

- `remaining == 0` plus UART `TC` is a lost software callback but a successful
  physical transmission;
- an unchanged non-zero count for the configured threshold is a stall and a
  failure, except while CTS can legitimately stop progress;
- zero remaining without `TC` gets a conservative, baud-derived budget for 64
  complete 12-bit hardware-pipeline symbols and then fails instead of holding
  the caller's span forever. This tail timeout is disabled under CTS.

RX health now requires the exact `HAL_UART_STATE_BUSY_RX` state whenever
software says it is armed. Every other state is debounced and recovered.

### 7.1 Abort-timeout repair

On H7RS, `HAL_UART_Abort*()` clears `DMAR`/`DMAT` before GPDMA may time out
waiting for suspend. A second UART abort then skips that DMA channel and can
leave its handle in `ERROR`. The driver now:

1. attempts the direction-specific or full UART abort;
2. returns success only when UART and DMA handle states are all truly ready;
3. calls `HAL_DMA_Init()` only if that direction's peripheral request bit is
   already clear;
4. retries the UART abort so its own handle state and flags finish transitioning;
5. never repairs the independent DMA direction while its request is live.

Three attempts cover TX repair, RX repair, and the final full-UART state
transition. Persistent failure keeps RX/TX storage owned and retries from
thread context; it never reports a false terminal event.

### 7.2 Baud transaction

`setBaudRate()` validates byte-transport mode before teardown, refuses a live
TX borrow, stops RX, makes stale callbacks inert, and applies the new rate. On
FIFO-capable USARTs it saves and checks FIFO mode plus both threshold fields.
Any configuration failure reapplies the previous baud and the complete saved
FIFO state. CTS stall policy is refreshed from the resulting `huart->Init`, RX
is restarted, and the receive discontinuity is reported as an ordered gap. If
configuration succeeded but the subsequent RX arm transiently fails, the
requested configuration stays applied and `proceed()` retries the receiver;
the false return means the requested link is not live yet.

## 8. Cortex-M code-generation evidence

Compiler: GNU Arm Embedded 14.3, Cortex-M4 soft-float, C++20,
`-Os -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections`.
Baseline and audited objects were built from the same SHA-separated worktrees
with the same compiler and flags.

| Artifact (`Uart<256,4>` unless noted) | Baseline | Audited tree | Change |
|---|---:|---:|---:|
| RX callback thunk | 96 B | 84 B | -12 B |
| RX callback stack frame | 16 B | 8 B | -8 B |
| TX callback thunk | 92 B | 80 B | -12 B |
| `receiveArm()` | 144 B | 108 B | -36 B |
| `publishActive()` | 54 B | 40 B | -14 B |
| `voidActiveChunk()` | 38 B | 28 B | -10 B |
| idle `proceed()` | 44 B | 36 B | -8 B |
| `isrError()` | 182 B | 158 B | -24 B |
| `healthCheck()` | 378 B | 358 B | -20 B |
| `sizeof(Uart<256,4>)` | 1696 B | 1664 B | -32 B |
| `sizeof(Uart<64,2>)` | 544 B | 512 B | -32 B |
| normal RX event-type HAL calls | 1 | 0 | removed |

The complete G4 portability-test object changed from 8,118 to 8,622 bytes of
text because the audit added bounded-count rejection, transactional recovery,
DMA repair, post-DMA TX liveness, and registry/configuration validation. Its
BSS fell from 4,696 to 4,600 bytes: three instantiated UART objects each
became 32 bytes smaller. This whole-object delta is reported honestly; the hot
functions themselves became smaller despite the new checks.

The old object placed `m_huart` around offset 1504 and `m_started` around 1508.
The audited object places them at offsets 0 and 16. This removes wide object
offset addressing from the repeatedly executed control accesses.

An `-O3 -DNDEBUG` ARM build is also required and compiled, but no cycle claim
is inferred from code size. The independent fresh DWT measurements are
recorded below.

## 9. Hardware evidence and optimization boundary

The original H7S3 CSVs remain the historical baseline for the architecture:
600 MHz M7, D-cache enabled, up to 10 Mbaud full duplex, with zero overrun,
UART error, restart, or gap across the recorded sweep. At 10 Mbaud the old
worst VCP-shaped full-duplex case consumed about 2.2% of one core, which is why
an LL backend was rejected.

### 9.1 Fresh audited-silicon run

On 2026-09-01 the audited working tree was rebuilt and run on the connected
NUCLEO-H7S3L8, STM32H7S3 revision Y, through its ST-Link V3 VCP. The target ran
at 600 MHz with I-cache and D-cache enabled; USART3 used GPDMA1 channel 11 for
RX and channel 10 for TX. Every ELF was compiled at `-Os` with GNU Arm 14.3,
downloaded with STM32CubeProgrammer 2.21, and accepted only after programmer
verification succeeded.

The reproduced and extended matrix was:

- historical `256x4` geometry at 115200, 1M, 3M, 6M, and 10 Mbaud;
- all six scenarios at every rate: idle, RX continuous, RX burst, TX
  continuous, full-duplex continuous, and full-duplex burst;
- live `setBaudRate()` transitions 115200 -> 3M -> 115200, each followed by a
  three-second continuous-RX proof;
- historical 10 Mbaud RX continuous/burst chunk sweep at `128x4`, `256x4`,
  and `512x4`;
- all six 10 Mbaud scenarios on the actual new default, `128x8`.

That is 40 recorded CSV windows plus two live-baud windows. Every one reported
`GAP=0`, `OVR=0`, `ERR=0`, and `RST=0`. The 30-window historical matrix is in
`results_256_audited_2026-09-01.csv`; the additional chunk/default runs are in
the three correspondingly named audited CSVs under `uart/tests/bench/`.

At 10 Mbaud, comparing the same `256x4` scenarios against the original sweep:

| Scenario | Audited hot averages versus old | CPU old -> audited |
|---|---|---:|
| RX continuous | RX 637 -> 619 cy | 1.788% -> 1.793% |
| RX burst | RX 628 -> 611 cy | 0.017% -> 0.016% |
| TX continuous | TX start 361 -> 360 cy; TX DMA IRQ 281 -> 277 cy | 1.948% -> 1.948% |
| duplex continuous | RX 626 -> 610 cy; TX start 362 -> 361 cy; TX DMA IRQ 281 -> 277 cy | 2.177% -> 2.180% |
| duplex burst | RX 625 -> 609 cy; TX start 361 -> 360 cy; TX DMA IRQ 281 -> 277 cy | 1.958% -> 1.960% |

The tiny total-CPU differences are run-to-run VCP shaping/event-count noise;
the per-call hot-path reductions are consistent with the ARM disassembly. On
the new `128x8` default at 10 Mbaud, RX continuous measured 592 RX cycles per
event and 1.750% CPU; duplex continuous measured 582 RX cycles per event and
2.185% CPU, again with all failure counters zero.

The chunk sweep retained the expected D-cache trend: `128x4`, `256x4`, and
`512x4` continuous RX measured 592, 619, and 675 RX cycles per event. The
hardware counters also directly confirm that HT stays disabled: at 10 Mbaud
continuous TX, `TXDMA.calls == TXFR == 159745`, not two DMA interrupts per
frame; at 1 Mbaud continuous RX, `RXDMA.calls == CH == 3870`.

`uart/tests/bench/README.md` documents the harness and CPU-accounting rule.

The following tempting changes remain rejected without contrary target data:

- LL/register-level duplicate backend;
- circular or linked-list RX;
- a hidden TX queue or copied TX frame;
- removing delegates or converting the driver to C;
- pre-invalidating RX storage only once (dirty cache eviction can overwrite DMA data);
- trusting the early HAL `size` instead of the frozen DMA count;
- atomic stats in every ISR;
- forcing large functions inline for a few speculative call cycles;
- changing registry architecture for a default table of four entries.

A field-layout prototype moved `TxHandler` into the 64-byte control prefix and
`Stats` to the tail. It kept total RAM unchanged but grew the three-instance G4
test object by 142 text bytes, grew `receiveArm()` by 4 bytes through its
overflow counter path, and did not reduce the complete TX thunk. It was
rejected; the measured control/Stats/storage order remains.

## 10. Verification matrix

### Host runtime

- strict GCC/WSL: 203 checks, zero failures;
- old-HAL `UART_ENGINE_HAS_RXEVENT_TYPE=0`: 203 checks, zero failures;
- `USE_HAL_UART_REGISTER_CALLBACKS=1`: 216 checks, zero failures;
- application-owned external callback forwarding: 203 checks, zero failures;
- `-O3 -DNDEBUG`: 203 checks, zero failures;
- ASan+UBSan, non-recovering: 203 checks, zero failures;
- ASan+UBSan targeted torture: seed `0xDEADBEEF`, 1,000,000 events survived;
- MinGW/UCRT strict, fallback, registered-callback, external-forwarding, and
  optimized variants: all pass (sanitizers intentionally skipped on MinGW);
- ten invalid macro/template configurations fail compilation with their
  intended diagnostics.

The runtime suite covers registry alias/null safety, structural init refusal,
IDLE/TC, stray HT, corrupt DMA counters, cache/ownership visibility, slot
conservation, ordered gaps, handler re-entry, arm/abort failures, exact single
TX terminal events, cross-direction abort races, single- and two-direction DMA
repair, DMA and post-DMA TX stalls, low-baud drain budgeting, lost completion,
CTS behavior, receiver watchdog recovery, transactional baud/FIFO rollback,
registered callbacks, and randomized interleavings.

### Real HAL compile/codegen

- STM32F103xE / Cortex-M3 / legacy SR+DR / classic DMA;
- STM32G474xx / Cortex-M4 / ISR+RDR / classic DMA;
- STM32H7S3xx / Cortex-M7 / D-cache / GPDMA;
- old RxEvent fallback, registered callbacks, and external forwarding;
- strict warnings as errors plus `-Wconversion`, `-Wsign-conversion`, and
  `-Wshadow`;
- GCC static analyzer over all instantiated G4 API paths;
- fixed ARM object-layout assertions;
- RX thunk size and stack budget gate;
- `UART_ENGINE_PROBE=0` disassembly exactly identical to a macro-stubbed build;
- G4 and H7RS probe-on builds instantiate the DWT backend.

### Repository integration

- the standalone `cobs.pri` qmake consumer builds and runs successfully;
- the complete Qt release application builds successfully with Qt 6.10.1 /
  MinGW 13.1;
- the COBS host suites pass on WSL/GCC and MinGW/UCRT, including the optimized
  variants; the WSL sanitizer and exhaustive decoder/encoder runs also pass;
- the independent Cortex-M COBS layout assertions pass with GNU Arm 14.3.

## 11. Residual contracts

These boundaries are required for deterministic embedded behavior:

1. The UART object and all HAL/DMA handles outlive active DMA. Static lifetime
   is recommended because a destructor cannot make disappearing storage safe
   after a permanently failed hardware abort.
2. The complete `Uart` object lives in DMA-accessible RAM. On M7 it must not be
   in DTCM. If an MPU makes the region non-cacheable,
   `UART_ENGINE_DCACHE_MAINTENANCE=0` must be configured consistently.
3. UART-global and RX-DMA IRQs use the same preemption priority unless the
   application accepts the documented rare lost-chunk race.
4. `proceed()` has one loop caller, and `send()` has one producer context.
   Volatile plus IRQ masking is the single-core ISR contract, not general
   multithread synchronization.
5. Handler targets obey the execution-context table, do not throw, and do not
   destroy themselves while active. Explicitly borrowed targets outlive the
   delegate.
6. `Stats` values are coherent snapshots but approximate counters. Concurrent
   ISR/thread increments may coalesce, and each value wraps modulo 2^32.
7. `setBaudRate()` can attempt rollback, but if the hardware refuses both the
   new and previous configuration the method returns false and recovery keeps
   retrying; software cannot guarantee repair of physically wedged hardware.
8. The application does not mutate HAL handle configuration behind the
   driver's back during operation. Supported reconfiguration goes through
   `setBaudRate()`.
9. TX spans are at most 65,535 bytes and remain alive until the one terminal
   ownership event.
10. A new performance number for this exact implementation requires the H7S
    DWT benchmark to be rebuilt, flashed, and rerun; the automated codegen
    gates prevent obvious regressions between board runs.

## 12. Automated closure criteria

- [x] Linux strict/fallback/registered/optimized runtime suites;
- [x] Linux ASan+UBSan non-recovering suite;
- [x] MinGW strict/fallback/registered/optimized runtime suites;
- [x] invalid configuration compile-fail diagnostics;
- [x] F1/G4/H7RS warning-clean compile matrix;
- [x] G4 static analyzer;
- [x] Cortex-M layout, RX code-size, and stack gates;
- [x] probe-off exact disassembly equivalence and probe-on builds;
- [x] baseline/current object and hot-symbol comparison;
- [x] COBS qmake consumer, complete Qt release build, and COBS regression suites;
- [x] canonical ownership, callback, configuration, and recovery contracts;
- [x] fresh NUCLEO-H7S3L8 115200-through-10M baud and chunk/default sweeps;
- [ ] isolated commit and push, when explicitly requested.
