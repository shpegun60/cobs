/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_RTU_TEST_H_
#define MODBUS_RTU_TEST_H_

#include "modbus/rtu/Crc.h"

#include <algorithm>
#include <cstdio>
#include <span>
#include <string_view>
#include <vector>

namespace modbus_test {

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

template<::crc::Policy CrcT>
inline std::vector<uint8_t> make_adu(
		CrcT& policy,
		const uint8_t address,
		const uint8_t function,
		const std::span<const uint8_t> data = {})
{
	std::vector<uint8_t> adu;
	adu.reserve(data.size() + 2u + CrcT::wire_size);
	adu.push_back(address);
	adu.push_back(function);
	adu.insert(adu.end(), data.begin(), data.end());
	const std::size_t body_size = adu.size();
	adu.resize(body_size + CrcT::wire_size);
	const typename CrcT::value_type value = policy.calculate(
		std::span<const uint8_t>{adu}.first(body_size));
	policy.store(adu.data() + body_size, value);
	return adu;
}

inline std::vector<uint8_t> make_adu(
		const uint8_t address,
		const uint8_t function,
		const std::span<const uint8_t> data = {})
{
	modbus::rtu::crc::Bitwise policy{};
	return make_adu(policy, address, function, data);
}

inline bool equal(
		const std::span<const uint8_t> left,
		const std::span<const uint8_t> right)
{
	return left.size() == right.size() &&
	       std::equal(left.begin(), left.end(), right.begin());
}

inline int finish()
{
	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}

} // namespace modbus_test

#endif /* MODBUS_RTU_TEST_H_ */
