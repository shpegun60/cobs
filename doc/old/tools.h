/*
 * tools.h
 *
 *  Created on: Nov 22, 2024
 *      Author: admin
 */

#ifndef CHAOS_PP_TOOLS_H_
#define CHAOS_PP_TOOLS_H_

#include "basic_types.h"
#include <limits>

namespace chaos
{

template <reg N>
static inline constexpr reg power_of_2() noexcept
{
	//static_assert(N <= 256, "Input must be less than 256");
	static_assert(N >= 0, "Input must be positive");
	static_assert((N & (N - 1)) == 0, "Input must be a power of 2");

	return N == 1 ? 0 : 1 + power_of_2<N / 2>();
}

/**
 * @brief Checks if a given number is a power of 2.
 *
 * @param x The number to check.
 * @return true if the number is a power of 2, false otherwise.
 */
static inline constexpr bool is_power_of_2(const reg x) noexcept
{
	return (x != 0) && ((x & (x - 1)) == 0);
}

static inline constexpr reg next_power_of_2(reg n) noexcept
{
	if (n == 0) {
		return 1;
	}

	--n;
	n |= n >> 1;  n |= n >> 2;
	n |= n >> 4;  n |= n >> 8;
	n |= n >> 16;

	if constexpr (std::numeric_limits<reg>::digits > 32) {
	    n |= n >> 32;
	}

	return n + 1;
}

static inline constexpr reg countr_zero(const reg x) noexcept {
	// Check if m_elementSize is a power of two
	if (!is_power_of_2(x)) {
		// Handle error or return a default value
		return 0; // or another appropriate value
	}

#if defined(__cpp_lib_bitops) // Check for C++20 support
	return std::countr_zero(x);
#elif defined(__GNUC__) // For GCC and Clang compilers
	return __builtin_ctzll(x);
#elif defined(_MSC_VER) // For Microsoft Visual Studio compiler
	unsigned long index;
	_BitScanForward64(&index, x);
	return static_cast<unsigned>(index);
#else
	// If specialized functions are not available, use a standard method
	reg shift 	= 0;
	reg size 	= x;

	while (size > 1) {
		size >>= 1;
		++shift;
	}
	return shift;
#endif
}

static inline constexpr bool is_arithmetic_right_shift() {
	constexpr int x = -1; 			// Binary: 1111...1111
	return (x >> 1) == -1; 			// Arithmetic shift: 1111...1111 (-1), logical: 0111...1111 (big positive)
}


} /* namespace chaos */

#endif /* CHAOS_PP_TOOLS_H_ */
