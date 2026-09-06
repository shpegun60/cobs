/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef CRC_TEST_H_
#define CRC_TEST_H_

#include <cstdio>
#include <string_view>

namespace crc_test {

inline int checks = 0;
inline int failures = 0;

inline void group(const char* const name)
{
	std::printf("\n[%s]\n", name);
}

inline void check(const bool condition, const std::string_view description)
{
	++checks;
	if (condition) {
		std::printf("  ok    %.*s\n", static_cast<int>(description.size()),
		            description.data());
	} else {
		++failures;
		std::printf("  FAIL  %.*s\n", static_cast<int>(description.size()),
		            description.data());
	}
}

inline int finish()
{
	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

} // namespace crc_test

#endif /* CRC_TEST_H_ */
