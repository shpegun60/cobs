/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/Pdu.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

using namespace modbus_test;

namespace {

enum class Code : uint16_t { Expected = 0xCAFEu };

struct Record {
	uint8_t byte;
	uint32_t word;
};

template<class T>
concept CanReadNative = requires(
		std::span<const uint8_t> bytes,
		std::size_t& offset,
		T& value) {
	modbus::read_native(bytes, offset, value);
};

template<class T>
concept CanReadBe = requires(
		std::span<const uint8_t> bytes,
		std::size_t& offset,
		T& value) {
	modbus::read_be(bytes, offset, value);
};

template<class T>
concept CanReadLe = requires(
		std::span<const uint8_t> bytes,
		std::size_t& offset,
		T& value) {
	modbus::read_le(bytes, offset, value);
};

} // namespace

int main()
{
	group("Constants");
	check(modbus::max_pdu_size == 253u, "PDU limit is 253 bytes");
	check(modbus::max_data_size == 252u, "function-data limit is 252 bytes");

	group("Readers");
	constexpr std::array<uint8_t, 12> bytes{
		0xA5u, 0x12u, 0x34u, 0x78u, 0x56u, 0x34u,
		0x12u, 0xCAu, 0xFEu, 0x10u, 0x20u, 0x30u};
	std::size_t offset = 0u;
	uint8_t one = 0u;
	uint16_t be16 = 0u;
	uint32_t le32 = 0u;
	Code code{};
	std::span<const uint8_t> tail{};
	check(modbus::read_be(bytes, offset, one) && one == 0xA5u && offset == 1u,
	      "one-byte read needs no special u8 function");
	check(modbus::read_be(bytes, offset, be16) && be16 == 0x1234u && offset == 3u,
	      "read_be assembles a 16-bit protocol value");
	check(modbus::read_le(bytes, offset, le32) && le32 == 0x12345678u && offset == 7u,
	      "read_le assembles a 32-bit protocol value");
	check(modbus::read_be(bytes, offset, code) && code == Code::Expected && offset == 9u,
	      "explicit-width enums use the same ordered reader");
	check(modbus::read_bytes(bytes, offset, 3u, tail) && offset == bytes.size() &&
	      tail.size() == 3u && tail[0] == 0x10u && tail[2] == 0x30u,
	      "read_bytes returns an immutable bounded subview");

	group("NativeReader");
	const uint32_t native_source = 0x89ABCDEFu;
	std::array<uint8_t, sizeof(native_source)> native_bytes{};
	std::memcpy(native_bytes.data(), &native_source, sizeof(native_source));
	std::size_t native_offset = 0u;
	uint32_t native_result = 0u;
	check(modbus::read_native(native_bytes, native_offset, native_result) &&
	      native_result == native_source && native_offset == sizeof(native_source),
	      "read_native preserves this target's scalar representation");
	constexpr std::array<uint8_t, 4> be_float_bytes{0x3Fu, 0xC0u, 0x00u, 0x00u};
	static_assert(sizeof(float) == 4u && std::numeric_limits<float>::is_iec559);
	std::size_t float_offset = 0u;
	float float_result = 0.0F;
	check(modbus::read_be(be_float_bytes, float_offset, float_result) &&
	      float_result == 1.5F && float_offset == sizeof(float),
	      "read_be supports an explicitly agreed IEEE-754 representation");

	group("FailureGuarantee");
	const std::size_t old_offset = offset;
	one = 0x77u;
	be16 = 0x8877u;
	le32 = 0xAABBCCDDu;
	const std::span<const uint8_t> old_tail = tail;
	check(!modbus::read_native(bytes, offset, one) &&
	      offset == old_offset && one == 0x77u,
	      "failed read_native changes neither cursor nor output");
	check(!modbus::read_be(bytes, offset, be16) &&
	      offset == old_offset && be16 == 0x8877u,
	      "failed read_be changes neither cursor nor output");
	check(!modbus::read_le(bytes, offset, le32) &&
	      offset == old_offset && le32 == 0xAABBCCDDu,
	      "failed read_le changes neither cursor nor output");
	check(!modbus::read_bytes(bytes, offset, 1u, tail) && offset == old_offset &&
	      tail.data() == old_tail.data() && tail.size() == old_tail.size(),
	      "failed read_bytes changes neither cursor nor output");

	group("TypeConstraints");
	static_assert(CanReadNative<uint32_t> && CanReadBe<uint32_t> && CanReadLe<uint32_t>);
	static_assert(CanReadNative<float> && CanReadBe<float> && CanReadLe<float>);
	static_assert(CanReadNative<Code> && CanReadBe<Code> && CanReadLe<Code>);
	static_assert(!CanReadNative<bool> && !CanReadBe<bool> && !CanReadLe<bool>);
	static_assert(!CanReadNative<Record> && !CanReadBe<Record> && !CanReadLe<Record>);
	static_assert(CanReadNative<long double> && !CanReadBe<long double> &&
	              !CanReadLe<long double>);
	check(true, "native and ordered readers enforce their documented scalar domains");

	return finish();
}
