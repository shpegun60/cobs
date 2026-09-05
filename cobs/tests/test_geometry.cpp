/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */
#include "Format.h"
#include <cstddef>
#include <cstdio>
#include <limits>

template<class Format>
bool verify_geometry()
{
	const std::size_t minimum = Format::tx_storage_size_for_capacity(0u);
	const std::size_t maximum = Format::tx_storage_size_for_capacity(Format::max_send_size);
	for (std::size_t bytes = 0u; bytes <= maximum + 512u; ++bytes) {
		const std::size_t capacity = Format::payload_capacity_for_storage(bytes);
		if (capacity > Format::max_send_size || (bytes < minimum && capacity != 0u)) {
			return false;
		}
		if (bytes >= minimum &&
		    (Format::tx_storage_size_for_capacity(capacity) > bytes ||
		     (capacity < Format::max_send_size &&
		      Format::tx_storage_size_for_capacity(capacity + 1u) <= bytes))) {
			std::printf("inverse failure: bytes=%zu, capacity=%zu\n", bytes, capacity);
			return false;
		}
	}
	return Format::payload_capacity_for_storage(std::numeric_limits<std::size_t>::max()) ==
	       Format::max_send_size;
}

int main()
{
	const bool ok = verify_geometry<cobs::Format<crc::NoCrc, 0>>() &&
		verify_geometry<cobs::Format<crc::NoCrc, 1>>() && verify_geometry<cobs::Format<crc::NoCrc, 254>>() &&
		verify_geometry<cobs::Format<crc::NoCrc, 255>>() && verify_geometry<cobs::Format<crc::NoCrc, 256>>() &&
		verify_geometry<cobs::Format<crc::NoCrc, 1024>>() && verify_geometry<cobs::Format<crc::NoCrc, 65535>>() &&
		verify_geometry<cobs::Format<crc::NoCrc, 65535, 1>>() &&
		verify_geometry<cobs::Format<>>() &&
		verify_geometry<cobs::Format<crc::Crc16Bitwise, 0>>() &&
		verify_geometry<cobs::Format<crc::Crc16Bitwise, 254>>() &&
		verify_geometry<cobs::Format<crc::Crc16Bitwise, 65533>>() &&
		verify_geometry<cobs::Format<crc::Crc64Bitwise, 65527>>();
	std::printf("COBS exact inverse geometry: %s\n", ok ? "passed" : "FAILED");
	return ok ? 0 : 1;
}
