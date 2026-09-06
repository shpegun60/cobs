<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Expected compile failures

Every `.cpp` file in this directory is intentionally invalid. They are not
library sources and must never be added to qmake. `check_compile_fail.sh`
compiles each one with `-fsyntax-only`, requires compilation to fail, and also
requires boundary-specific diagnostic markers.

| Case | Contract it locks |
|---|---|
| `storage_missing_tx.cpp` | `Endpoint` rejects an incomplete `Storage` |
| `message_private_encode.cpp` | encoding remains coordinator-only |
| `packet_private_adopt.cpp` | only `detail::Receiver` can create a packet reference |
| `append_struct.cpp` | native serialization rejects padded/ABI-defined structs |
| `legacy_get_msg.cpp` | the old message factory has no compatibility alias |
| `legacy_set_sender.cpp` | transport delegates cannot be installed separately |

A case that fails for an unrelated reason is a test failure, not a pass. Add a
diagnostic marker to the runner whenever a new negative contract is added.
