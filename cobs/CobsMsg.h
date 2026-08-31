/*
 * CobsMsg — exclusive owner of one TX block, and deliberately nothing more.
 *
 * Contract: doc/COBS_ENGINE.md §8. This type knows its own storage geometry
 * and how to encode into it. It knows nothing about a transport: `tx_busy()`,
 * `send()` and the single active transfer belong to the layer above, which is
 * why pushing is `cobs.push(msg)` rather than `msg.push()`. Keeping the
 * transport out means this is not templated on it — less template code, less
 * coupling, and a message that can be built and encoded with no link present.
 *
 * States:
 *
 *     Empty  --acquire-->  Building  --encode()-->  Encoded
 *
 * `Transferred` is not stored. When the layer above accepts the frame it
 * takes the block and the message returns to Empty: the same contract, one
 * fewer state to keep in RAM.
 *
 * Layout (§8.3) — the application never sees the headroom:
 *
 *      block
 *      |<-- raw_offset -->|<---- MaxDecodedSize ---->|
 *      +------------------+--------------------------+
 *      | encoder headroom | reserve() hands this out |
 *      +------------------+--------------------------+
 *      |<---------------- wire_capacity ------------>|
 *
 * After encode() the same block holds the wire frame from byte 0. No copy is
 * made at any point.
 */

#ifndef COBS_MSG_H_
#define COBS_MSG_H_

#include "CobsEncoder.h"

#include <cstddef>
#include <cstdint>
#include <span>

// One template parameter, like everything else in this layer: the policy
// states tx_max_size, so the geometry follows from it (COBS_ENGINE.md §9.2).
template<class Allocator>
class CobsMsg final {
public:
	static constexpr std::size_t max_payload_size = Allocator::tx_max_size;
	// The block holds the worst-case wire frame; the payload starts far
	// enough in for the encoder to overlap it safely (§8.4).
	static constexpr std::size_t wire_capacity = cobs_max_wire_size(max_payload_size);
	static constexpr std::size_t raw_offset    = cobs_raw_offset(max_payload_size);

	CobsMsg() noexcept = default;

	// Takes one block from the policy. Exhaustion yields an Empty message
	// rather than a failure code: `if (!msg)` is the check either way.
	explicit CobsMsg(Allocator& allocator) noexcept
		: m_pool(&allocator), m_block(allocator.allocate_tx())
	{
		if (m_block != nullptr) {
			m_state = State::Building;
		}
	}

	~CobsMsg() { release(); }

	CobsMsg(const CobsMsg&) = delete;
	CobsMsg& operator=(const CobsMsg&) = delete;

	CobsMsg(CobsMsg&& other) noexcept
		: m_pool(other.m_pool), m_block(other.m_block),
		  m_size(other.m_size), m_state(other.m_state)
	{
		other.disown();
	}

	CobsMsg& operator=(CobsMsg&& other) noexcept
	{
		if (this != &other) {
			release(); // whatever this message held is returned first
			m_pool = other.m_pool;
			m_block = other.m_block;
			m_size = other.m_size;
			m_state = other.m_state;
			other.disown();
		}
		return *this;
	}

	[[nodiscard]] explicit operator bool() const noexcept { return m_block != nullptr; }

	// The application's payload area. Returns an empty span if the message
	// owns no block, is already encoded, or the size exceeds the protocol
	// limit; reserving zero bytes is legal and also yields an empty span, so
	// consult payload_size() if the two must be told apart.
	[[nodiscard]] std::span<uint8_t> reserve(const std::size_t size) noexcept
	{
		if (m_state != State::Building || size > max_payload_size) {
			return {};
		}
		m_size = size;
		return {raw(), size};
	}

	[[nodiscard]] std::size_t payload_size() const noexcept { return m_size; }
	[[nodiscard]] bool encoded() const noexcept { return m_state == State::Encoded; }

	/*
	 * Encodes the reserved payload in place and returns the wire frame.
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
		if (m_state == State::Encoded) {
			return {raw() - raw_offset, m_wire};
		}
		const auto frame = cobs_encode_in_place(
			std::span<uint8_t>{raw() - raw_offset, wire_capacity}, raw_offset, m_size);
		if (frame.empty()) {
			return {}; // geometry is checked at compile time, so unreachable
		}
		m_wire = frame.size();
		m_state = State::Encoded;
		return frame;
	}

private:
	enum class State : uint8_t { Empty, Building, Encoded };

	[[nodiscard]] uint8_t* raw() const noexcept
	{
		return reinterpret_cast<uint8_t*>(m_block) + raw_offset;
	}

	void release() noexcept
	{
		if (m_block != nullptr) {
			m_pool->deallocate_tx(m_block);
		}
		disown();
	}

	void disown() noexcept
	{
		m_pool = nullptr;
		m_block = nullptr;
		m_size = 0;
		m_wire = 0;
		m_state = State::Empty;
	}

	Allocator*  m_pool  = nullptr;
	std::byte*  m_block = nullptr;
	std::size_t m_size  = 0; // reserved payload bytes
	std::size_t m_wire  = 0; // encoded frame length, once Encoded
	State       m_state = State::Empty;
};

#endif /* COBS_MSG_H_ */
