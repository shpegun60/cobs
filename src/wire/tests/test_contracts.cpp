/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "contract_checks.h"
#include "extension_checks.h"
#include <cstdio>

int main()
{
	unsigned checks = 0u, failures = 0u;
	const auto check = [&](bool ok) { ++checks; if (!ok) { ++failures; } };
	contract_checks::readers(check);
	contract_checks::policies<wire::Heap>(check);
	contract_checks::policies<wire::Pool<2u, 2u>>(check);
	extension_checks::run(check);
	std::printf("Reader/CRC/storage/framer call contracts: %u checks, %u failures\n", checks, failures);
	return failures == 0u ? 0 : 1;
}
