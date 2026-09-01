<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS hot-path benchmarks

Run from the repository root:

```sh
sh cobs/tests/bench/run.sh
```

`COBS_BENCH_MIB` selects the approximate payload volume per timing sample;
the default is 16 MiB. For a lower-noise comparison:

```sh
COBS_BENCH_MIB=64 sh cobs/tests/bench/run.sh
```

The runner builds with `-O3 -DNDEBUG -march=native` and reports the best of
five samples. `codec_bench` measures the two byte loops directly across four
payload patterns. `endpoint_bench` measures complete fixed-pool RX
(decode/validate/queue/pop/release) and TX
(acquire/build/encode/delegate/poll/release) paths.

Timing is deliberately not a pass/fail test: scheduler state, CPU frequency,
compiler version and code placement all move microbenchmarks. Compare two
revisions on the same machine, compiler and flags, alternating their run order.
Correctness belongs to `cobs/tests/run.sh`, sanitizers and the exhaustive
differential codec test.
