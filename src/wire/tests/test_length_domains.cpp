/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "length_checks.h"
#include <cstdio>
#include <cstdlib>

int main() {
    unsigned checks = 0;
    length_checks::run([&](bool okay) {
        ++checks;
        if (!okay) { std::fprintf(stderr, "Length domain check %u failed\n", checks); std::abort(); }
    });
    std::printf("Length domains: %u checks, 786426 TCP headers + 262144 RTU count declarations, exact OOM skip, no failures\n", checks);
}
