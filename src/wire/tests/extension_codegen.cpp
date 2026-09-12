/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Complete built-in endpoint paths for before/after object comparisons.
// Default: Pool<2,2>; -DPROBE_HEAP=1 selects Heap. No board/HAL dependency.
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#ifdef PROBE_HEAP
using Memory = wire::Heap;
#else
using Memory = wire::Pool<2, 2>;
#endif
using C = cobs::Endpoint<Memory>;
using R = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>,
    modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Request>>;
using T = modbus::tcp::Endpoint<Memory>;
extern "C" {
void c_rx(C* e, const uint8_t* p, std::size_t n) { e->consume({p, n}); }
void r_rx(R* e, const uint8_t* p, std::size_t n) { e->consume({p, n}); }
void t_rx(T* e, const uint8_t* p, std::size_t n) { e->consume({p, n}); }
wire::SendResult c_tx(C* e, C::Message* m) { return e->send(*m); }
wire::SendResult r_tx(R* e, R::Message* m) { return e->send(*m); }
wire::SendResult t_tx(T* e, T::Message* m) { return e->send(*m); }
void reclaim(C* c, R* r, T* t) { c->poll(0); r->poll(0); t->poll(0); }
extern const unsigned layout_sizes[] = {
    sizeof(C), sizeof(R), sizeof(T), sizeof(C::Storage), sizeof(R::Storage), sizeof(T::Storage),
    sizeof(C::Message), sizeof(R::Message), sizeof(T::Message), sizeof(C::Packet), sizeof(R::Packet), sizeof(T::Packet)
};
}
