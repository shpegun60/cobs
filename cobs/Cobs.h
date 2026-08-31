/*
 * Cobs — the assembled engine. RX vertical, TX vertical, one memory policy.
 *
 * Contract: doc/COBS_ENGINE.md. Everything difficult already lives one layer
 * down; what is here is the transport handshake and the single active
 * transfer.
 *
 *     RX:  bytes -> CobsDecoder -> policy -> RxPacket -> PacketRef
 *     TX:  CobsMsg -> encoder -> sender delegate -> activeTx -> policy
 *
 * ---------------------------------------------------------------------------
 * LIFETIME PRECONDITIONS. Not decoration — violating either is a use-after-
 * free that no amount of internal bookkeeping can repair:
 *
 *  1. A Cobs object must OUTLIVE every PacketRef and every CobsMsg it handed
 *     out. Both hold a pointer to the policy living inside this object, and
 *     both call back into it when they release. That is also why Cobs is
 *     neither copyable nor movable: moving it would turn every outstanding
 *     packet's owner pointer into a souvenir of a previous life.
 *
 *  2. A Cobs object must not be destroyed while a transfer is in flight
 *     (`tx_active()` true and the transport still busy). The transport is
 *     physically reading a block that is about to stop existing. Check
 *     `tx_active()` and drain with `proceed()` before letting it go.
 * ---------------------------------------------------------------------------
 */

#ifndef COBS_H_
#define COBS_H_

#include "CobsHeapAllocator.h"
#include "CobsMsg.h"
#include "CobsRx.h"
#include "PacketRef.h"

#include "tiny_delegate.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

enum class SendResult : uint8_t {
	Sent,     // the transport accepted the frame; Cobs now holds the block
	Busy,     // a transfer is already in flight; the message is untouched
	NotBound, // no sender / tx_busy delegate has been set
	Error,    // the transport refused to start; the message stays Encoded
	Invalid,  // the message owns no block, or belongs to another engine
};

template<class Allocator = CobsHeapAllocator<>>
class Cobs final {
public:
	using AllocatorType = Allocator;
	using Msg = CobsMsg<Allocator>;
	using Ref = PacketRef<Allocator>;

	// Republished so callers have one place to ask, and so a change of policy
	// is visible rather than implied (§4.1).
	static constexpr std::size_t max_decoded_size = Allocator::rx_max_size;
	static constexpr std::size_t max_send_size    = Allocator::tx_max_size;

	using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
	using TxBusy = tiny::delegate<bool()>;

	using RxStats = typename CobsRx<Allocator>::Stats;
	struct TxStats {
		uint32_t frames_sent       = 0;
		uint32_t send_refused_busy = 0;
		uint32_t send_failed       = 0;
	};

	Cobs() noexcept = default;

	// For a policy that needs runtime arguments — an external memory region, a
	// handle to somebody else's arena. The policy is constructed in place from
	// them, so it need not be copyable or movable (§9.4). There is
	// deliberately no constructor taking a ready-made policy: it would only
	// serve copyable ones, and one way in is enough.
	template<class... Args>
	explicit Cobs(std::in_place_t, Args&&... args) noexcept
		: m_allocator(std::forward<Args>(args)...) {}

	Cobs(const Cobs&) = delete;
	Cobs& operator=(const Cobs&) = delete;
	Cobs(Cobs&&) = delete;
	Cobs& operator=(Cobs&&) = delete;

	~Cobs()
	{
		// Precondition 2 above says this must not happen with the transport
		// still reading. If a block is here at all, the pool is about to be
		// destroyed with it, so returning it only keeps the pool's own
		// accounting honest for anything watching.
		if (m_activeTx.memory != nullptr) {
			m_allocator.deallocate_tx(m_activeTx.memory, m_activeTx.wire_size);
			m_activeTx = {};
		}
	}

	/* ------------------------------- setup ------------------------------- */

	/*
	 * Both delegates, together, or neither. Two independent setters made a
	 * genuine DMA mine reachable while the link was IDLE, so the previous
	 * "no rebinding during a transfer" guard was necessary but not enough:
	 *
	 *     set_sender(uartB);            // and the tx_busy setter is skipped,
	 *                                   // fails, or simply comes later
	 *     -> sender = B, tx_busy = A
	 *     push()    starts DMA on B
	 *     proceed() asks A, which is idle
	 *     -> the block is freed while B is still reading it
	 *
	 * A pairing this important is not something to document and hope for.
	 * Taking both at once makes the mixed states unrepresentable, and the
	 * half-bound ones too: a set sender with an empty tx_busy is refused.
	 *
	 * Two empty delegates are a clean unbind. Refused outright while a
	 * transfer is in flight — the old transport is still reading the active
	 * block, and a new tx_busy() answering "idle" would have proceed() free it
	 * out from under the DMA.
	 */
	[[nodiscard]] bool set_transport(Sender sender, TxBusy tx_busy) noexcept
	{
		if (m_activeTx.memory != nullptr) {
			return false;
		}
		if (static_cast<bool>(sender) != static_cast<bool>(tx_busy)) {
			return false; // half a transport is worse than none
		}
		m_sender = static_cast<Sender&&>(sender);
		m_txBusy = static_cast<TxBusy&&>(tx_busy);
		return true;
	}

	/* --------------------------------- RX -------------------------------- */

	void consume(std::span<const uint8_t> bytes) noexcept { m_rx.consume(bytes); }
	void gap() noexcept { m_rx.gap(); }

	[[nodiscard]] Ref pop_packet() noexcept { return m_rx.pop_packet(); }
	[[nodiscard]] bool has_packet() const noexcept { return m_rx.has_packet(); }

	/* --------------------------------- TX -------------------------------- */

	/*
	 * A message whose block is sized for exactly this payload — the point of
	 * the sized TX contract (§9.1): a seven-byte frame costs nine bytes on a
	 * heap policy rather than the worst case of the largest frame allowed.
	 *
	 * Callers who cannot know the length until they have serialized it ask for
	 * an upper bound and then truncate():
	 *
	 *     auto msg = cobs.get_msg(known_size);
	 *     auto data = msg.payload();
	 *
	 *     auto msg = cobs.get_msg(upper_bound);
	 *     const auto actual = serialize(msg.payload());
	 *     (void)msg.truncate(actual);
	 *
	 * An EMPTY result is the back-pressure signal that the configured TX
	 * storage is exhausted, and callers must handle it; CobsMsg is already a
	 * nullable owner, so there is nothing for an optional to add.
	 */
	[[nodiscard]] Msg get_msg(const std::size_t payload_size) noexcept
	{
		if (payload_size > max_send_size) {
			return {};
		}
		return Msg{m_allocator, payload_size};
	}

	/*
	 * Takes the message by REFERENCE, not by value or rvalue, because on
	 * every outcome except Sent the caller keeps it and retries (§8.1):
	 *
	 *   Busy   -> still Building, raw payload untouched and still writable
	 *   Error  -> still Encoded, and push() may retry the SAME wire frame
	 *   Sent   -> the block moved here; the message is Empty again
	 */
	[[nodiscard]] SendResult push(Msg& msg) noexcept
	{
		if (!msg || !msg.belongs_to(m_allocator)) {
			return SendResult::Invalid;
		}
		if (!m_sender || !m_txBusy) {
			return SendResult::NotBound;
		}
		if (m_activeTx.memory != nullptr || m_txBusy()) {
			++m_txStats.send_refused_busy;
			return SendResult::Busy;
		}

		const auto wire = msg.encode();
		if (wire.empty()) {
			return SendResult::Invalid;
		}
		if (!m_sender(wire)) {
			++m_txStats.send_failed;
			return SendResult::Error; // message stays Encoded, frame retryable
		}

		// Ownership moves only AFTER the transport has accepted the frame:
		// until then the message must remain able to retry it.
		m_activeTx = msg.surrender_block();
		++m_txStats.frames_sent;
		return SendResult::Sent;
	}

	[[nodiscard]] bool tx_active() const noexcept { return m_activeTx.memory != nullptr; }

	/* ------------------------------ main loop ---------------------------- */

	// Reclaims the transmitted block once the transport lets go. An idle link
	// short-circuits on the null pointer and never invokes the delegate at
	// all, which is what makes the delegate's cost irrelevant (§2.1).
	//
	// tx_busy() == false means only that the transport stopped borrowing the
	// buffer — never that the frame was delivered (§8.2). Delivery outcome is
	// the transport's business, reported through its own counters.
	void proceed() noexcept
	{
		if (m_activeTx.memory != nullptr && !m_txBusy()) {
			// The ALLOCATION size travels with the pointer, so a policy that
			// segregates by size class knows where it belongs without
			// searching (§9.1).
			m_allocator.deallocate_tx(m_activeTx.memory, m_activeTx.wire_size);
			m_activeTx = {};
		}
	}

	/* ----------------------------- observation --------------------------- */

	[[nodiscard]] const RxStats& rx_stats() const noexcept { return m_rx.stats(); }
	[[nodiscard]] const TxStats& tx_stats() const noexcept { return m_txStats; }
	// Const on purpose: statistics and geometry are worth reading, but a
	// mutable reference would let a caller allocate behind the engine's back
	// and hand out blocks it never learns about.
	[[nodiscard]] const Allocator& allocator() const noexcept { return m_allocator; }

private:
	[[no_unique_address]] Allocator m_allocator{};
	CobsRx<Allocator> m_rx{m_allocator};

	Sender m_sender{};
	TxBusy m_txBusy{};

	typename Msg::TxBlock m_activeTx{};
	TxStats m_txStats{};
};

#endif /* COBS_H_ */
