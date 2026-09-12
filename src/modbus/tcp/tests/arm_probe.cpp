/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/tcp/Tcp.h"
#ifndef TCP_PROBE_CRC
#define TCP_PROBE_CRC 0
#endif
using Crc = std::conditional_t<TCP_PROBE_CRC == 0, crc::NoCrc,
	std::conditional_t<TCP_PROBE_CRC == 1, crc::Crc16Bitwise, crc::Crc16Table>>;
using E = modbus::tcp::Endpoint<wire::Pool<2, 2>, modbus::tcp::Format<Crc>>;
static_assert(sizeof(E::Packet) == sizeof(void*));
static_assert(E::Geometry::alignment == alignof(void*));
static_assert(E::max_send_size == 252u && E::max_receive_size == 252u);
static_assert(E::Geometry::rx_block_bytes == wire::round_up(276u + Crc::wire_size, 4u));
static_assert(E::Geometry::tx_block_bytes == 260u + Crc::wire_size);
using EB = modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::Crc16Bitwise>>;
using ET = modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::Crc16Table>>;
static_assert(sizeof(EB) == sizeof(ET));
static_assert(std::same_as<EB::Storage, ET::Storage> && std::same_as<EB::Packet, ET::Packet> && std::same_as<EB::Message, ET::Message>);
extern "C" void tcp_consume(E* endpoint, const uint8_t* bytes, std::size_t size)
{ endpoint->consume({bytes, size}); }
extern "C" wire::SendResult tcp_send(E* endpoint, E::Message* message)
{ return endpoint->send(*message); }
extern "C" uint16_t tcp_transaction(const E::Packet* packet) { return packet->transaction_id(); }
extern "C" bool tcp_append(E::Message* message, uint32_t value) { return message->append_be(value); }
