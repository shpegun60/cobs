/*
 * CobsMsg — exclusive owner of one TX block, and a builder for what goes in it.
 *
 * Contract: doc/COBS_ENGINE.md §8. This type knows its own storage geometry,
 * how to grow it, and how to encode into it. It knows nothing about a
 * transport: tx_busy(), send() and the single active transfer belong to the
 * layer above, which is why pushing is cobs.push(msg) rather than msg.push().
 * Keeping the transport out means this is not templated on it — less template
 * code, less coupling, and a message that can be built with no link present.
 *
 * States:
 *
 *     Empty  --make_msg-->  Building  --encode()-->  Encoded
 *
 * Transferred is not stored. When the layer above accepts the frame it takes
 * the block and the message returns to Empty: the same contract, one fewer
 * state to keep in RAM.
 *
 * ---------------------------------------------------------------------------
 * SIZE AND CAPACITY, as in any container:
 *
 *      size()      payload bytes actually WRITTEN so far
 *      capacity()  payload bytes the current block permits
 *
 * A message starts at size 0 and is filled with write<T>(), write_bytes() and
 * write_array(). Capacity grows on demand, geometrically (~1.5x), so a caller
 * that cannot predict the length simply writes until it is done. The optional
 * argument to make_msg() is a capacity HINT, never an initial size.
 *
 * There is deliberately no writable payload span in the public API. Handing
 * one out would mean handing out a pointer that the next write() may
 * invalidate, and the whole point of the builder is that ordinary callers
 * never meet that hazard.
 *
 * POINTER INVALIDATION, for the code that reaches past the builder anyway:
 * any operation that increases capacity() may move the payload. Writes that
 * fit do not, and encode() never does.
 * ---------------------------------------------------------------------------
 *
 * Layout (§8.3), with K the capacity of the current block:
 *
 *      block, cobs_max_wire_size(K) bytes
 *      |<-- cobs_raw_offset(K) -->|<------------- K ------------->|
 *      +--------------------------+-------------------------------+
 *      | encoder headroom         | payload area                  |
 *      +--------------------------+---------------+---------------+
 *                                 |<-- size() --->|
 *
 * The payload is PHYSICALLY at cobs_raw_offset(K), and moving it at encode
 * time would end the zero-copy story, so encoding a payload of size S simply
 * starts further in:
 *
 *      |<-- unused -->|<-- R(S) -->|<------ S ------>|
 *      ^              ^
 *      block          encoding begins here, and so does the wire frame
 *
 * It fits exactly: R(S) <= R(K) because S <= K, and the encoded region ends at
 * cobs_raw_offset(K) + S, which is at most the end of the block. So the frame
 * need not start at block[0]; the transport is handed the span encode()
 * returns, while the ALLOCATION remains the whole block and is returned as
 * such. Encoding copies nothing.
 */

#ifndef COBS_MSG_H_
#define COBS_MSG_H_

#include "CobsEncoder.h"
#include "TxAllocation.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

// The only legitimate destination for a surrendered block.
template<class Allocator>
class Cobs;

/*
 * What write<T>() and write_array<T>() accept: the types that have a meaning
 * on a wire rather than merely a representation in this process.
 *
 * A struct is deliberately NOT one of them, even though C++ would happily copy
 * its object representation. `struct { uint8_t a; uint32_t b; }` would go out
 * with three bytes of padding, in this compiler's field order and this
 * target's byte order, and the receiver would have to reproduce all three
 * accidents to read it. A comment saying so protects only the people who read
 * comments; a constraint stops the code from compiling.
 *
 * `bool` is excluded for the same reason: `true` is allowed to be any non-zero
 * bit pattern, so its object representation is the compiler's private
 * business. Write `uint8_t{flag ? 1u : 0u}` and the wire says what you meant.
 *
 * Pointers and member pointers fall out automatically: neither is arithmetic
 * nor an enumeration.
 *
 * If raw object representation is ever genuinely needed, it belongs behind a
 * deliberately alarming name — write_object_representation() — and not behind
 * the method everybody reaches for first.
 */
template<class T>
concept CobsScalar =
	(std::is_arithmetic_v<std::remove_cv_t<T>> &&
	 !std::is_same_v<std::remove_cv_t<T>, bool>) ||
	std::is_enum_v<std::remove_cv_t<T>> ||
	std::is_same_v<std::remove_cv_t<T>, std::byte>;

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
	// return, and the capacity to return it WITH — the same number the policy
	// reported at allocation, so a segregated policy finds its size class
	// without searching. Kept together so that surrendering a block without
	// it is not expressible.
	struct TxBlock {
		std::byte*  memory   = nullptr;
		std::size_t capacity = 0;
	};

	CobsMsg() noexcept = default;

	/*
	 * Takes a block from the policy and starts EMPTY. `hint` is a capacity
	 * hint, not a size: it saves the first growth for a caller who knows
	 * roughly how much is coming, and nothing more.
	 *
	 * Exhaustion, or a hint beyond the policy's declared limit, yields an
	 * Empty message rather than a failure code: `if (!msg)` is the check
	 * either way.
	 */
	explicit CobsMsg(Allocator& allocator, const std::size_t hint = 0) noexcept
		: m_pool(&allocator)
	{
		if (hint > max_payload_size) {
			return;
		}
		const TxAllocation allocation = allocator.allocate_tx(hint);
		if (allocation.memory != nullptr) {
			m_block = allocation.memory;
			m_capacity = allocation.capacity;
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

	[[nodiscard]] std::size_t size() const noexcept { return m_size; }
	[[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
	[[nodiscard]] bool encoded() const noexcept { return m_state == State::Encoded; }

	/* ------------------------------ building ----------------------------- */

	/*
	 * Appends one scalar, enumeration or std::byte AS STORED ON THIS TARGET.
	 * No byte swapping happens here or anywhere else in this layer: protocol
	 * endianness is the caller's business, and a hidden swap in a transport
	 * library is a bug that only shows up on the second architecture.
	 *
	 * What is NOT accepted, and why, is in the CobsScalar concept above.
	 *
	 * Returns false, with the message completely unchanged, if the value does
	 * not fit the policy's limit or the growth it needs cannot be allocated.
	 */
	template<CobsScalar T>
	[[nodiscard]] bool write(const T& value) noexcept
	{
		// A COMPILE-TIME length, deliberately: the copy then becomes a store
		// or two rather than a call into memcpy, which is what a serializer
		// that runs per field has to be.
		return append_fixed<sizeof(T)>(&value);
	}

	// Raw bytes, appended as they are.
	[[nodiscard]] bool write_bytes(const std::span<const uint8_t> bytes) noexcept
	{
		return append(bytes.data(), bytes.size());
	}

	/*
	 * A contiguous run of values, appended as one block of object
	 * representations — no length prefix, deliberately. A caller that needs
	 * one writes it, which keeps the protocol's own framing visible in the
	 * protocol's own code:
	 *
	 *     msg.write<uint16_t>(count);
	 *     msg.write_array(values);
	 */
	template<CobsScalar T>
	[[nodiscard]] bool write_array(const std::span<const T> values) noexcept
	{
		if (values.size() > max_payload_size / sizeof(T)) {
			return false; // checked before the multiply, so it cannot overflow
		}
		return append(reinterpret_cast<const uint8_t*>(values.data()),
		              values.size() * sizeof(T));
	}

	/*
	 * Makes room for `required` payload bytes in total, growing if needed.
	 * Rarely called directly — the write methods call it — but useful to a
	 * caller that knows the final length and wants the single allocation up
	 * front.
	 *
	 * On failure NOTHING changes: the old block, its capacity, its size and
	 * its contents all survive intact. That is what lets a caller treat a
	 * false return as "this message cannot carry that" rather than as "this
	 * message is now in an unknown state".
	 */
	[[nodiscard]] bool reserve(const std::size_t required) noexcept
	{
		if (m_state != State::Building) {
			return false;
		}
		if (required <= m_capacity) {
			return true; // the common case: no allocation, nothing moves
		}
		if (required > max_payload_size) {
			return false;
		}

		const TxAllocation fresh = m_pool->allocate_tx(grow_target(m_capacity, required));
		if (fresh.memory == nullptr) {
			return false; // strong guarantee: the old block is untouched
		}

		// The payload's physical offset depends on the capacity, so this is a
		// copy between two different offsets — the one memcpy in the whole TX
		// vertical, and only on an actual growth.
		if (m_size != 0u) {
			std::memcpy(reinterpret_cast<uint8_t*>(fresh.memory) + cobs_raw_offset(fresh.capacity),
			            raw(), m_size);
		}
		m_pool->deallocate_tx(m_block, m_capacity); // returned with its OWN capacity
		m_block = fresh.memory;
		m_capacity = fresh.capacity;
		return true;
	}

	// Guards against pushing a message into a DIFFERENT engine of the same
	// type, which would return the block to the wrong pool. The types cannot
	// catch that: two Cobs<CobsFixedAllocator<...>> objects are one type.
	[[nodiscard]] bool belongs_to(const Allocator& allocator) const noexcept
	{
		return m_pool == &allocator;
	}

	/* ------------------------------ encoding ----------------------------- */

	/*
	 * Encodes the payload in place and returns the wire frame. Building ends
	 * here: writes and reserve() are refused afterwards, because the raw bytes
	 * they would append to no longer exist.
	 *
	 * Idempotent: calling it on an already-encoded message returns the same
	 * frame without touching the block. That is what lets the layer above
	 * retry a transfer whose hardware start failed — once encoding has run
	 * there is nothing to undo, so the message stays Encoded and the SAME wire
	 * frame is sent again.
	 *
	 * Returns an empty span if there is no block to encode.
	 */
	[[nodiscard]] std::span<const uint8_t> encode() noexcept
	{
		if (m_block == nullptr) {
			return {};
		}
		// The payload sits physically at cobs_raw_offset(m_capacity); a
		// smaller logical size needs only cobs_raw_offset(m_size) of headroom,
		// so the frame begins further into the block rather than at its start.
		// Moving the payload instead would be a copy, which is the one thing
		// this path refuses to do.
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
	 * The next capacity to ask for: about 1.5x the current one, but never less
	 * than what is actually required and never more than the policy allows.
	 *
	 *      0 -> 1 -> 2 -> 3 -> 4 -> 6 -> 9 -> 13 -> 19 -> ...
	 *
	 * A shift and an add, deliberately: 1.5x needs no division, which on a
	 * Cortex-M0 would be a call to __aeabi_uidiv for the privilege of saving a
	 * few bytes. The `max(1, ...)` keeps small capacities moving, since 1 >> 1
	 * is 0 and a growth of zero would loop forever.
	 *
	 * A large jump is honoured in ONE allocation rather than by walking the
	 * sequence: from 64, a request for 500 asks for 500, not 96 then 144 then
	 * 216 and so on.
	 */
	[[nodiscard]] static constexpr std::size_t grow_target(const std::size_t capacity,
	                                                       const std::size_t required) noexcept
	{
		const std::size_t half = capacity >> 1;
		const std::size_t delta = (half == 0u) ? 1u : half;
		const std::size_t headroom = max_payload_size - capacity; // capacity <= max
		const std::size_t grown = capacity + ((delta > headroom) ? headroom : delta);
		const std::size_t target = (required > grown) ? required : grown;
		return (target > max_payload_size) ? max_payload_size : target;
	}

	// Bounds and growth, shared by every write. Leaves the message untouched
	// on any failure, which is what makes a false return safe to ignore.
	[[nodiscard]] bool make_room(const std::size_t n) noexcept
	{
		if (m_state != State::Building) {
			return false;
		}
		if (n > max_payload_size - m_size) { // m_size <= max_payload_size always
			return false;
		}
		return reserve(m_size + n);
	}

	// The scalar path: N is a constant, so the copy compiles to stores.
	template<std::size_t N>
	[[nodiscard]] bool append_fixed(const void* const src) noexcept
	{
		if (!make_room(N)) {
			return false;
		}
		std::memcpy(raw() + m_size, src, N); // raw() AFTER any reallocation
		m_size += N;
		return true;
	}

	// The bulk path: a runtime length, where a call into memcpy is the right
	// answer rather than a regression.
	[[nodiscard]] bool append(const uint8_t* const src, const std::size_t n) noexcept
	{
		if (!make_room(n)) {
			return false;
		}
		if (n != 0u) {
			std::memcpy(raw() + m_size, src, n);
		}
		m_size += n;
		return true;
	}

	/*
	 * Hands the block to the layer above WITHOUT freeing it: the transport is
	 * about to borrow it, and the message must stop owning it at the same
	 * instant. Leaves the message Empty. Only called after the transport has
	 * already accepted the frame (§8.2).
	 *
	 * The capacity travels with the pointer because it is what the policy
	 * reported for THIS block — which a growth may have changed since the
	 * message was created, and which is not the logical size and not a frame
	 * length. Recomputing it later would hand the policy a different block
	 * than it gave out.
	 */
	[[nodiscard]] TxBlock surrender_block() noexcept
	{
		const TxBlock block{m_block, m_capacity};
		m_block = nullptr; // so disown() cannot free what the transport holds
		disown();
		return block;
	}

	enum class State : uint8_t { Empty, Building, Encoded };

	[[nodiscard]] uint8_t* raw() const noexcept
	{
		return reinterpret_cast<uint8_t*>(m_block) + cobs_raw_offset(m_capacity);
	}

	void release() noexcept
	{
		if (m_block != nullptr) {
			m_pool->deallocate_tx(m_block, m_capacity);
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
	std::size_t m_capacity = 0; // payload bytes this block permits
	std::size_t m_size     = 0; // payload bytes written; <= m_capacity
	std::size_t m_wire     = 0; // encoded frame length, once Encoded
	State       m_state    = State::Empty;
};

#endif /* COBS_MSG_H_ */
