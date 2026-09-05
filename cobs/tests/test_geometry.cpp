/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Layout::payload_capacity_for_storage must be the EXACT inverse of
 * tx_storage_size_for_capacity, for every physical grant a storage could make:
 * the largest payload capacity whose worst-case frame still fits, clamped to
 * the Format's send limit, and zero below the empty frame's size. Checked by
 * brute force over every byte count up to well past the maximum, on formats
 * spanning both header widths, every CRC width in use, and the 16-bit edge.
 */
#include "Format.h"

#include <cstddef>
#include <cstdio>
#include <limits>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const bool ok, const char* const what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what);
	}
}

template<class Format>
void verify_geometry(const char* const name)
{
	const std::size_t minimum = Format::tx_storage_size_for_capacity(0u);
	const std::size_t maximum = Format::tx_storage_size_for_capacity(Format::max_send_size);
	bool exact = true;
	for (std::size_t bytes = 0u; bytes <= maximum + 512u; ++bytes) {
		const std::size_t capacity = Format::payload_capacity_for_storage(bytes);
		if (capacity > Format::max_send_size || (bytes < minimum && capacity != 0u)) {
			exact = false;
		}
		if (bytes >= minimum &&
		    (Format::tx_storage_size_for_capacity(capacity) > bytes ||
		     (capacity < Format::max_send_size &&
		      Format::tx_storage_size_for_capacity(capacity + 1u) <= bytes))) {
			if (exact) {
				std::printf("  inverse failure in %s: bytes=%zu, capacity=%zu\n", name, bytes, capacity);
			}
			exact = false;
		}
	}
	check(exact, name);
	check(Format::payload_capacity_for_storage(std::numeric_limits<std::size_t>::max()) ==
	          Format::max_send_size,
	      name);
	std::printf("  ok    %s: exact inverse for every grant 0..%zu, clamped at SIZE_MAX\n",
	            name, maximum + 512u);
}

} // namespace

int main()
{
	std::printf("\n[InverseGeometry]\n");
	verify_geometry<cobs::Format<crc::NoCrc, 0>>("NoCrc/0");
	verify_geometry<cobs::Format<crc::NoCrc, 1>>("NoCrc/1");
	verify_geometry<cobs::Format<crc::NoCrc, 254>>("NoCrc/254 (H1 edge)");
	verify_geometry<cobs::Format<crc::NoCrc, 255>>("NoCrc/255 (legacy v1)");
	verify_geometry<cobs::Format<crc::NoCrc, 256>>("NoCrc/256 (H2)");
	verify_geometry<cobs::Format<crc::NoCrc, 1024>>("NoCrc/1024");
	verify_geometry<cobs::Format<crc::NoCrc, 65535>>("NoCrc/65535");
	verify_geometry<cobs::Format<crc::NoCrc, 65535, 1>>("NoCrc/65535,1 (asymmetric)");
	verify_geometry<cobs::Format<>>("default CRC16/253");
	verify_geometry<cobs::Format<crc::Crc16Bitwise, 0>>("CRC16/0");
	verify_geometry<cobs::Format<crc::Crc16Bitwise, 254>>("CRC16/254 (H2 threshold)");
	verify_geometry<cobs::Format<crc::Crc16Bitwise, 65533>>("CRC16/65533 (16-bit edge)");
	verify_geometry<cobs::Format<crc::Crc64Bitwise, 65527>>("CRC64/65527 (16-bit edge)");

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
