/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#if TEST_CRC == 0
using Integrity = crc::NoCrc;
#elif TEST_CRC == 1
using Integrity = crc::Crc16Bitwise;
#else
using Integrity = crc::Crc16Table;
#endif

#if TEST_COBS
using Link = cobs::Endpoint<wire::Heap, cobs::Format<Integrity>>;
extern "C" void shared_cobs_rx(Link& link, std::span<const uint8_t> bytes) { link.consume(bytes); }
extern "C" cobs::SendResult shared_cobs_tx(Link& link, Link::Message& msg) { return link.send(msg); }
#else
using Link = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<Integrity>>;
extern "C" void shared_rtu_rx(Link& link, std::span<const uint8_t> bytes) { link.receive_adu(bytes); }
extern "C" modbus::SendResult shared_rtu_tx(Link& link, Link::Message& msg) { return link.send(msg); }
#endif
