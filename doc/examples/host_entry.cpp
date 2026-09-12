/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Host fixture only: put the fake peripheral model before application globals
// in ONE translation unit. C++ then guarantees their reverse destruction order:
// static UARTs shut down while fake HAL maps/vectors are still alive. Depending
// on linker order across separate TUs made the old examples fail under ASan.
// Neither fake_hal.cpp nor this wrapper belongs in firmware.
#include "fake_hal.cpp"
#ifndef DOC_EXAMPLE_SOURCE
#error "host_entry.cpp is built by doc/examples/build.sh, with a selected source"
#endif
#include DOC_EXAMPLE_SOURCE
