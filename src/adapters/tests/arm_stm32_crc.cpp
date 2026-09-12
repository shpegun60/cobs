/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "adapters/stm32/Crc16.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include <type_traits>

using Hardware = crc::stm32::Crc16;
using CobsHardware = cobs::Endpoint<wire::Heap, cobs::Format<Hardware>>;
using RtuHardware = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<Hardware>>;
static_assert(!std::is_default_constructible_v<Hardware>);
static_assert(std::is_nothrow_constructible_v<Hardware, CRC_HandleTypeDef&>);
static_assert(std::is_same_v<CobsHardware::Storage, cobs::Endpoint<>::Storage>);
static_assert(std::is_same_v<CobsHardware::Message, cobs::Endpoint<>::Message>);
static_assert(std::is_same_v<CobsHardware::Packet, cobs::Endpoint<>::Packet>);
static_assert(std::is_same_v<RtuHardware::Storage, modbus::rtu::Endpoint<>::Storage>);
static_assert(std::is_same_v<RtuHardware::Message, modbus::rtu::Endpoint<>::Message>);
static_assert(std::is_same_v<RtuHardware::Packet, modbus::rtu::Endpoint<>::Packet>);

extern "C" uint16_t hardware_crc(CRC_HandleTypeDef& handle, const uint8_t* data, std::size_t size) noexcept
{
    return Hardware{handle}.calculate({data, size});
}
