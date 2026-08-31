/*
 * CobsMsg — exclusive owner of one TX block, and deliberately nothing more.
 *
 * Contract: doc/COBS_ENGINE.md §8. This type knows its own storage geometry
 * and how to encode into it. It knows nothing about a transport: tx_busy(),
 * send() and the single active transfer belong to the layer above, which is
 * why pushing is cobs.push(msg) rather than msg.push(). Keeping the transport
 * out means this is not templated on it — less template code, less coupling,
 * and a message that can be built and encoded with no link present.
 *
 * States:
 *
 *     Empty  --acquire-->  Building  --encode()-->  Encoded
 *
 * Transferred is not stored. When the layer above accepts the frame it takes
 * the block and the message returns to Empty: the same contract, one fewer
 * state to keep in RAM.
 *
 * The block is sized for the payload the caller ASKED for, not for the largest
 * the policy could ever carry — a seven-byte message costs nine bytes on a
 * heap policy. Layout (§8.3), with C the requested capacity:
 *
 *      block, cobs_max_wire_size(C) bytes
 *      |<-- cobs_raw_offset(C) -->|<--------- C ---------->|
 *      +--------------------------+------------------------+
 *      | encoder headroom         | payload() hands this   |
 *      +--------------------------+------------------------+
 *
 * truncate() then splits two geometries that used to be one. The payload is
 * already PHYSICALLY at cobs_raw_offset(C), and moving it would end the
 * zero-copy story, so encoding a shortened payload S simply starts further in:
 *
 *      |<-- unused -->|<-- R(S) -->|<------ S ------>|
 *      ^              ^
 *      block          encoding begins here, and so does the wire frame
 *
 * It fits exactly: R(S) <= R(C) because S <= C, and the encoded region ends at
 * cobs_raw_offset(C) + S, which is at most the end of the block. So the frame
 * need not start at block[0]; the transport is handed the span encode()
 * returns, while the ALLOCATION remains the whole block and is returned as
 * such. No copy is made at any point.
 */

#ifndef COBS_MSG_H_
#define COBS_MSG_H_

#include "CobsEncoder.h"

#include <cstddef>
#include <cstdint>
#include <span>

// The only legitimate destination for a surrendered block.
template<class Allocator>
class Cobs;

// One template parameter, like everything else in this layer: the policy
// states tx_max_size, so the geometry follows from it (COBS_ENGINE.md §9.2).
template<class Allocator>
class CobsMsg final {
	// surrender_block() is private for the same reason PacketRef::adopt() is:
	// it hands out ownership without freeing it, so exactly one type may call
	// it — the one that will keep the block alive until the transport is done.
	friend class Cobs<Allocator>;

public:
	static constexpr std::size_t max_payload_size = Allocator::tx_max_size;

	// What Cobs holds while the transport borrows the frame: the block to
	// return, and the size to return it WITH. Kept together so that
	// surrendering a block without its size is not expressible.
	struct TxBlock {
		std::byte*  memory    = nullptr;
		std::size_t wire_size = 0;
	};

	CobsMsg() noexcept = default;

	// Takes a block sized for exactly this payload. Exhaustion, or a capacity
	// beyond the policy's declared limit, yields an Empty message rather than
	// a failure code: `if (!msg)` is the check either way.
	CobsMsg(Allocator& allocator, const std::size_t capacity) noexcept
		: m_pool(&allocator)
	{
		if (capacity > max_payload_size) {
			return;
		}
		m_block = allocator.allocate_tx(cobs_max_wire_size(capacity));
		if (m_block != nullptr) {
			m_capacity = capacity;
			m_size = capacity;
			m_state = State::Building;
		}
	}

	~CobsMsg() { release(); }

	CobsMsg(const CobsMsg&) = delete;
	CobsMsg& operator=(const CobsMsg&) = delete;

	CobsMsg(CobsMsg&& other) noexcept
		: m_pool(other.m_pool), m_block(other.m_block),
		  m_capacity(other.m_capacity), m_size(other.m_size),
		  m_wire(other.m_wire), m_state(other.m_state)
	{
		other.disown();
	}

	CobsMsg& operator=(CobsMsg&& other) noexcept
	{
		if (this != &other) {
			release(); // whatever this message held is returned first
			m_pool = other.m_pool;
			m_block = other.m_block;
			m_capacity = other.m_capacity;
			m_size = other.m_size;
			m_wire = other.m_wire;
			m_state = other.m_state;
			other.disown();
		}
		return *this;
	}

	[[nodiscard]] explicit operator bool() const noexcept { return m_block != nullptr; }

	// The application's payload area: the capacity it asked for, or what is
	// left of it after truncate(). Empty once encoded — the raw bytes are gone
	// by then.
	[[nodiscard]] std::span<uint8_t> payload() noexcept
	{
		if (m_state != State::Building) {
			return {};
		}
		return {raw(), m_size};
	}

	/*
	 * Shrinks the payload to what was actually written, for callers that
	 * cannot know the length until they have serialized it: ask for an upper
	 * bound, write, then truncate.
	 *
	 * Monotonically downwards, deliberately. `truncate` means shrink, and
	 * allowing 100 -> 20 -> 80 would let a caller resurrect bytes it had
	 * already disclaimed — which somebody would eventually try.
	 */
	[[nodiscard]] bool truncate(const std::size_t size) noexcept
	{
		if (m_state != State::Building || size > m_size) {
			return false;
		}
		m_size = size;
		return true;
	}

	[[nodiscard]] std::size_t payload_size() const noexcept { return m_size; }
	[[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
	[[nodiscard]] bool encoded() const noexcept { return m_state == State::Encoded; }

	// Guards against pushing a message into a DIFFERENT engine of the same
	// type, which would return the block to the wrong pool. The types cannot
	// catch that: two Cobs<CobsFixedAllocator<...>> objects are one type.
	[[nodiscard]] bool belongs_to(const Allocator& allocator) const noexcept
	{
		return m_pool == &allocator;
	}

	/*
	 * Encodes the payload in place and returns the wire frame.
	 *
	 * Idempotent: calling it on an already-encoded message returns the same
	 * frame without touching the block. That is what lets the layer above
	 * retry a transfer whose hardware start failed — once encoding has run,
	 * the raw payload no longer exists and there is nothing to undo, so the
	 * message stays Encoded and the SAME wire frame is sent again.
	 *
	 * Returns an empty span if there is no block to encode.
	 */
	[[nodiscard]] std::span<const uint8_t> encode() noexcept
	{
		if (m_block == nullptr) {
			return {};
		}
		// The payload sits physically at cobs_raw_offset(m_capacity); a
		// truncated one needs only cobs_raw_offset(m_size) of headroom, so the
		// frame begins further into the block rather than at its start.
		// Moving the payload instead would be a copy, which is the one thing
		// this design refuses to do.
		const std::size_t enc_offset = cobs_raw_offset(m_size);
		uint8_t* const begin = raw() - enc_offset;

		if (m_state == State::Encoded) {
			return {begin, m_wire};
		}
		const auto frame = cobs_encode_in_place(
			std::span<uint8_t>{begin, cobs_max_wire_size(m_size)}, enc_offset, m_size);
		if (frame.empty()) {
			return {}; // the geometry above makes this unreachable
		}
		m_wire = frame.size();
		m_state = State::Encoded;
		return frame;
	}

private:
	/*
	 * Hands the block to the layer above WITHOUT freeing it: the transport is
	 * about to borrow it, and the message must stop owning it at the same
	 * instant. Leaves the message Empty. Only called after the transport has
	 * already accepted the frame (§8.2).
	 *
	 * The size travels with the pointer because it is the ALLOCATION size —
	 * cobs_max_wire_size(capacity) — not the possibly-truncated frame length.
	 * Recomputing it later from m_size would hand back a smaller block than
	 * was taken, which is exactly how sized deallocation gets broken.
	 */
	[[nodiscard]] TxBlock surrender_block() noexcept
	{
		const TxBlock block{m_block, allocation_size()};
		m_block = nullptr; // so disown() cannot free what the transport holds
		disown();
		return block;
	}

	enum class State : uint8_t { Empty, Building, Encoded };

	[[nodiscard]] std::size_t allocation_size() const noexcept
	{
		return cobs_max_wire_size(m_capacity);
	}

	[[nodiscard]] uint8_t* raw() const noexcept
	{
		return reinterpret_cast<uint8_t*>(m_block) + cobs_raw_offset(m_capacity);
	}

	void release() noexcept
	{
		if (m_block != nullptr) {
			m_pool->deallocate_tx(m_block, allocation_size());
		}
		disown();
	}

	void disown() noexcept
	{
		m_pool = nullptr;
		m_block = nullptr;
		m_capacity = 0;
		m_size = 0;
		m_wire = 0;
		m_state = State::Empty;
	}

	Allocator*  m_pool     = nullptr;
	std::byte*  m_block    = nullptr;
	std::size_t m_capacity = 0; // payload capacity asked for; fixes the block
	std::size_t m_size     = 0; // payload after truncate(); <= m_capacity
	std::size_t m_wire     = 0; // encoded frame length, once Encoded
	State       m_state    = State::Empty;
};

#endif /* COBS_MSG_H_ */
