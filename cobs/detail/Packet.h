/*
 * Packet — the application's handle on a decoded RX packet.
 *
 * Contract: doc/COBS_ENGINE.md §6.3–§6.5. An intrusive shared handle: the
 * count lives inside the block, release goes through the storage instance
 * the block already names, and the whole thing is typed on that storage at
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

#ifndef COBS_DETAIL_PACKET_H_
#define COBS_DETAIL_PACKET_H_

#include "../Storage.h"

#include <cstddef>
#include <span>
#include <utility>

namespace cobs {

template<class StorageT>
class Packet final {
	// adopt() and the raw pointer are deliberately NOT public. With both
	// `adopt(Block*)` and a public `get()`, an application could write
	//
	//     Packet b = Packet::adopt(a.get());
	//
	// and end up with two RAII owners of the SAME single reference: the first
	// destructor frees the block, the second is left holding a dangling
	// pointer. A hand-operated use-after-free factory in the public API.
	template<class>
	friend class detail::Receiver;

public:
	using Block = typename StorageT::RxBlock;

	Packet() noexcept = default;
	~Packet() { release(); }

	Packet(const Packet& other) noexcept : m_p(other.m_p)
	{
		if (m_p != nullptr) {
			++m_p->refs;
		}
	}

	Packet(Packet&& other) noexcept : m_p(other.m_p)
	{
		other.m_p = nullptr; // a move never touches the count
	}

	Packet& operator=(const Packet& other) noexcept
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

	Packet& operator=(Packet&& other) noexcept
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

private:
	// Takes over an EXISTING reference without touching the count: the one
	// held by the ready queue becomes the one held here (§6.3). Named, never
	// implicit, and reachable only by the owner that legitimately has a
	// reference to hand over.
	[[nodiscard]] static Packet adopt(Block* const p) noexcept
	{
		Packet r;
		r.m_p = p;
		return r;
	}

	void release() noexcept
	{
		if (m_p != nullptr && --m_p->refs == 0u) {
			m_p->owner->release_rx(m_p);
		}
	}

	Block* m_p = nullptr;
};

} // namespace cobs

#endif /* COBS_DETAIL_PACKET_H_ */
