/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace modbus_test;

namespace {

class Capture final {
public:
	[[nodiscard]] bool send(const std::span<const uint8_t> bytes) noexcept
	{
		frame.assign(bytes.begin(), bytes.end());
		borrowed = true;
		return true;
	}

	[[nodiscard]] bool busy() const noexcept { return borrowed; }

	bool borrowed = false;
	std::vector<uint8_t> frame{};
};

template<
	class PolicyT,
	class StorageT = modbus::rtu::Pool<2, 1>>
void round_trip(const char* const name)
{
	using Endpoint = modbus::rtu::Endpoint<StorageT, PolicyT>;
	using Format = typename Endpoint::Format;

	group(name);
	Endpoint endpoint;
	Capture capture;
	check(endpoint.bind(
		typename Endpoint::Sender{tiny::bind<&Capture::send>(capture)},
		typename Endpoint::BusyQuery{tiny::bind<&Capture::busy>(capture)}),
		"policy endpoint binds with no runtime selection");

	std::vector<uint8_t> data(Endpoint::max_send_size);
	for (std::size_t index = 0u; index < data.size(); ++index) {
		data[index] = static_cast<uint8_t>(index * 37u + PolicyT::wire_size);
	}

	auto message = endpoint.make_message(0xF7u, 0x64u, data.size());
	check(message && message.capacity() == Endpoint::max_send_size &&
	      message.append_bytes(data),
	      "full Pool slab exposes exactly the policy-derived data capacity");
	check(endpoint.send(message) == modbus::SendResult::Sent &&
	      capture.frame.size() == Endpoint::max_frame_size &&
	      ::crc::verify<PolicyT>(capture.frame),
	      "maximum payload becomes one valid exact 256-byte ADU");

	endpoint.receive_adu(capture.frame);
	auto packet = endpoint.pop_packet();
	check(packet && packet.address() == 0xF7u && packet.function() == 0x64u &&
	      packet.size() == Endpoint::max_receive_size &&
	      equal(packet.data(), data) && packet.pdu().size() == data.size() + 1u &&
	      packet.adu().size() == Endpoint::max_frame_size,
	      "RX views use the same policy-derived offsets and sizes");
	packet.reset();

	check(Endpoint::crc_size == PolicyT::wire_size &&
	      Endpoint::max_send_size ==
	          modbus::rtu::max_adu_size - 2u - PolicyT::wire_size &&
	      Format::adu_size_for_data(Endpoint::max_send_size) ==
	          modbus::rtu::max_adu_size,
	      "Endpoint geometry is a compile-time function of policy wire_size");

	capture.borrowed = false;
	endpoint.poll();
}

struct Sum24 final : ::crc::Codec<
	uint32_t, 3u, std::endian::little> {
	[[nodiscard]] uint32_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		uint32_t value = 0u;
		for (const uint8_t byte : bytes) {
			value = (value + static_cast<uint32_t>(byte)) & 0x00FFFFFFu;
		}
		return value;
	}
};

struct HardwareState final {
	unsigned calls = 0u;
	const void* last_handle = nullptr;
};

struct FakeHardware32 final : ::crc::Codec<
	uint32_t, 4u, std::endian::little> {
	FakeHardware32(HardwareState& observed, const void* peripheral) noexcept
		: state(&observed), handle(peripheral) {}
	FakeHardware32() = delete;

	[[nodiscard]] uint32_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		++state->calls;
		state->last_handle = handle;
		return ::crc::Crc32Bitwise{}.calculate(bytes);
	}

	HardwareState* state;
	const void* handle;
};

class SpyStorage final {
public:
	using RxBlock = modbus::rtu::RxBlock<SpyStorage>;
	static constexpr std::size_t max_adu_size = modbus::rtu::max_adu_size;

	[[nodiscard]] RxBlock* acquire_rx(std::size_t) noexcept { return nullptr; }
	void release_rx(RxBlock*) noexcept {}

	[[nodiscard]] modbus::rtu::TxBlock acquire_tx(
			const std::size_t requested) noexcept
	{
		m_last_request = requested;
		if (m_in_use || requested > m_memory.size()) {
			return {};
		}
		m_in_use = true;
		return {m_memory.data(), requested};
	}

	void release_tx(const modbus::rtu::TxBlock block) noexcept
	{
		if (block.memory == m_memory.data()) {
			m_in_use = false;
		}
	}

	[[nodiscard]] std::size_t last_request() const noexcept
	{
		return m_last_request;
	}

private:
	std::array<std::byte, max_adu_size> m_memory{};
	std::size_t m_last_request = 0u;
	bool m_in_use = false;
};

static_assert(modbus::rtu::Storage<SpyStorage>);

static_assert(modbus::rtu::Format<::crc::NoCrc::wire_size>::crc_size == 0u);
static_assert(modbus::rtu::Format<::crc::NoCrc::wire_size>::min_adu_size == 2u);
static_assert(modbus::rtu::Format<::crc::NoCrc::wire_size>::max_data_size == 254u);
static_assert(modbus::rtu::Format<::crc::Crc8Bitwise::wire_size>::max_data_size == 253u);
static_assert(modbus::rtu::Format<::crc::Crc16Bitwise::wire_size>::max_data_size == 252u);
static_assert(modbus::rtu::Format<Sum24::wire_size>::max_data_size == 251u);
static_assert(modbus::rtu::Format<::crc::Crc32Bitwise::wire_size>::max_data_size == 250u);
static_assert(modbus::rtu::Format<::crc::Crc64Bitwise::wire_size>::max_data_size == 246u);

} // namespace

int main()
{
	round_trip<::crc::Crc8Bitwise>("CRC8BitwiseGeometry");
	round_trip<::crc::Crc8Table>("CRC8TableGeometry");
	round_trip<::crc::Crc16Bitwise>("CRC16BitwiseGeometry");
	round_trip<::crc::Crc16Table>("CRC16TableGeometry");
	round_trip<Sum24>("CustomThreeByteGeometry");
	round_trip<::crc::Crc32Bitwise>("CRC32BitwiseGeometry");
	round_trip<::crc::Crc32Table>("CRC32TableGeometry");
	round_trip<::crc::Crc64Bitwise>("CRC64BitwiseGeometry");
	round_trip<::crc::Crc64Table>("CRC64TableGeometry");
	round_trip<::crc::NoCrc>("NoCrcGeometry");
	round_trip<::crc::NoCrc, modbus::rtu::Heap>("NoCrcHeapGeometry");

	group("StatefulHardwarePolicy");
	using HardwareEndpoint = modbus::rtu::Endpoint<
		modbus::rtu::Pool<1, 1>, FakeHardware32>;
	static_assert(!std::is_default_constructible_v<HardwareEndpoint>);
	HardwareState hardware_state;
	const uint32_t peripheral_token = 0x12345678u;
	HardwareEndpoint hardware{
		FakeHardware32{hardware_state, &peripheral_token}};
	Capture hardware_capture;
	check(hardware.bind(
		HardwareEndpoint::Sender{
			tiny::bind<&Capture::send>(hardware_capture)},
		HardwareEndpoint::BusyQuery{
			tiny::bind<&Capture::busy>(hardware_capture)}),
		"non-default hardware policy is injected through Endpoint constructor");
	auto hardware_message = hardware.make_message(1u, 3u, 2u);
	const std::array<uint8_t, 2u> hardware_data{0x12u, 0x34u};
	check(hardware_message.append_bytes(hardware_data) &&
	      hardware.send(hardware_message) == modbus::SendResult::Sent &&
	      hardware_state.calls == 1u &&
	      hardware_state.last_handle == &peripheral_token,
	      "TX calls the stateful peripheral instance and uses its four-byte codec");
	hardware.receive_adu(hardware_capture.frame);
	auto hardware_packet = hardware.pop_packet();
	check(hardware_packet && equal(hardware_packet.data(), hardware_data) &&
	      hardware_state.calls == 2u &&
	      hardware_state.last_handle == &peripheral_token,
	      "RX reuses the exact same stateful peripheral policy instance");
	hardware_packet.reset();
	hardware_capture.borrowed = false;
	hardware.poll();

	group("NoCrcSemantics");
	using NoCrcEndpoint = modbus::rtu::Endpoint<
		modbus::rtu::Pool<2, 1>, ::crc::NoCrc>;
	NoCrcEndpoint no_crc;
	const std::array<uint8_t, 2u> minimum{0x01u, 0x03u};
	no_crc.receive_adu(minimum);
	auto packet = no_crc.pop_packet();
	check(packet && packet.data().empty() && packet.adu().size() == 2u,
	      "NoCrc accepts the two-byte address/function minimum with no trailer");
	packet.reset();
	std::array<uint8_t, 3u> arbitrary{0x01u, 0x03u, 0xA5u};
	no_crc.receive_adu(arbitrary);
	packet = no_crc.pop_packet();
	check(packet && packet.data().size() == 1u && packet.data()[0] == 0xA5u &&
	      no_crc.stats().rx.crc_errors == 0u,
	      "NoCrc intentionally performs no integrity validation");

	group("StorageIndependence");
	using Pool = modbus::rtu::Pool<2, 1>;
	using DefaultEndpoint = modbus::rtu::Endpoint<Pool>;
	using NoCrcLink = modbus::rtu::Endpoint<Pool, ::crc::NoCrc>;
	using Crc64Link = modbus::rtu::Endpoint<Pool, ::crc::Crc64Table>;
	check(sizeof(NoCrcLink) == sizeof(DefaultEndpoint) &&
	      sizeof(Crc64Link) == sizeof(DefaultEndpoint) &&
	      std::is_same_v<typename NoCrcLink::StorageType,
	                     typename Crc64Link::StorageType>,
	      "empty CRC policies reuse the exact same Pool type and add no object RAM");

	group("PolicyToStorageBoundary");
	modbus::rtu::Endpoint<SpyStorage, ::crc::NoCrc> no_crc_spy;
	auto no_crc_message = no_crc_spy.make_message(1u, 3u, 10u);
	check(no_crc_message && no_crc_message.capacity() == 10u &&
	      no_crc_spy.storage().last_request() == 12u,
	      "NoCrc converts a 10-byte data hint into a 12-byte physical request");
	no_crc_message = {};
	modbus::rtu::Endpoint<SpyStorage, ::crc::Crc64Bitwise> crc64_spy;
	auto crc64_message = crc64_spy.make_message(1u, 3u, 10u);
	check(crc64_message && crc64_message.capacity() == 10u &&
	      crc64_spy.storage().last_request() == 20u,
	      "CRC64 converts the same hint into a 20-byte physical request");

	return finish();
}
