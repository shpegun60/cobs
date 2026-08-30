/*
 * PacketRef — the application's handle on a decoded RX packet.
 *
 * Contract: doc/COBS_ENGINE.md §6.3–§6.5. An intrusive shared handle: the
 * count lives inside the packet, the deallocation goes through the allocator
 * the packet already names, and the whole thing is typed on the allocator at
 * compile time. There is deliberately no void* owner and no deleter function
 * pointer, because storing those would be re-implementing shared_ptr inside
 * the packet — which is the thing this design exists to avoid.
 *
 * The count is a plain integer. One execution domain needs nothing more; an
 * atomic policy belongs to the day cross-task sharing actually appears, not
 * to a speculative today.
 *
 * The application sees the payload only as a const span: the bytes are
 * immutable once published.
 */

#ifndef COBS_PACKET_REF_H_
#define COBS_PACKET_REF_H_

#include "RxPacket.h"

#include <cstddef>
#include <span>
#include <utility>

template<class Allocator>
class PacketRef final {
public:
	using Packet = RxPacket<Allocator>;

	PacketRef() noexcept = default;

	// Takes over an EXISTING reference without touching the count: the
	// reference held by the ready queue becomes the one held here (§6.3).
	// Named, and never implicit, so an ownership transfer is always visible
	// at the call site.
	[[nodiscard]] static PacketRef adopt(Packet* const p) noexcept
	{
		PacketRef r;
		r.m_p = p;
		return r;
	}

	~PacketRef() { release(); }

	PacketRef(const PacketRef& other) noexcept : m_p(other.m_p)
	{
		if (m_p != nullptr) {
			++m_p->refs;
		}
	}

	PacketRef(PacketRef&& other) noexcept : m_p(other.m_p)
	{
		other.m_p = nullptr; // a move never touches the count
	}

	PacketRef& operator=(const PacketRef& other) noexcept
	{
		// Increment BEFORE releasing: with self-assignment both sides are the
		// same packet, and releasing first could free what we then adopt.
		if (other.m_p != nullptr) {
			++other.m_p->refs;
		}
		release();
		m_p = other.m_p;
		return *this;
	}

	PacketRef& operator=(PacketRef&& other) noexcept
	{
		if (this != &other) {
			release();
			m_p = other.m_p;
			other.m_p = nullptr;
		}
		return *this;
	}

	void reset() noexcept
	{
		release();
		m_p = nullptr;
	}

	explicit operator bool() const noexcept { return m_p != nullptr; }

	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return (m_p != nullptr) ? m_p->data() : std::span<const uint8_t>{};
	}
	[[nodiscard]] std::size_t size() const noexcept
	{
		return (m_p != nullptr) ? m_p->size : 0u;
	}

	// For the COBS layer, not the application: the packet itself.
	[[nodiscard]] Packet* get() const noexcept { return m_p; }

private:
	void release() noexcept
	{
		if (m_p != nullptr && --m_p->refs == 0u) {
			m_p->owner->deallocate(m_p);
		}
	}

	Packet* m_p = nullptr;
};

#endif /* COBS_PACKET_REF_H_ */
