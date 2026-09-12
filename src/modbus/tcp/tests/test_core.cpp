/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "core_cases.h"
#include <cstdio>
#include <cstdlib>

template<class Crc>
unsigned run()
{
	unsigned checks = 0u;
	for (const auto result : {tcp_test::core_cases<wire::Heap, Crc>(), tcp_test::core_cases<wire::Pool<4, 2>, Crc>()}) {
		if (result.failed_line != 0u) { std::fprintf(stderr, "core_cases.h:%u FAILED after %u checks\n", result.failed_line, result.checks); std::exit(1); }
		checks += result.checks;
	}
	return checks;
}
int main()
{
	const unsigned checks = run<crc::NoCrc>() + run<crc::Crc8Bitwise>() + run<crc::Crc8Table>() +
		run<crc::Crc16Bitwise>() + run<crc::Crc16Table>() + run<crc::Crc32Bitwise>() + run<crc::Crc32Table>() +
		run<crc::Crc64Bitwise>() + run<crc::Crc64Table>();
	std::printf("TCP core: %u checks, Heap/Pool, all nine built-in policies\n", checks);
}
