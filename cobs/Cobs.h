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

#include "CobsFrameFormat.h"
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

	/*
	 * Republished so callers have one place to ask, and so a change of policy
	 * is visible rather than implied (§4.1). Both are BODY limits — the
	 * largest application payload this instance will receive and send. The
	 * decoded frame is length_size bytes longer than either, which is why
	 * neither is called max_decoded_size any more: that name was one header
	 * away from the truth, on a layer where being one header out is the
	 * easiest mistake there is.
	 */
	static constexpr std::size_t max_receive_size = Allocator::rx_max_size;
	static constexpr std::size_t max_send_size    = Allocator::tx_max_size;

	/*
	 * The wire format this engine speaks (COBS_ENGINE.md §3). Every frame
	 * carries a fixed-width decoded length prefix, and its width is chosen
	 * from the LARGER of the two limits so that one engine uses one header
	 * width in both directions:
	 *
	 *     length_size = max(rx_max_size, tx_max_size) <= 255 ? 1 : 2
	 *
	 * Peers must agree on this exactly as they agree on a baud rate; two that
	 * disagree cannot exchange even a one-byte frame. It is constexpr so an
	 * integration build can static_assert the format it expects.
	 */
	using Format = CobsFormatFor<Allocator>;
	static constexpr std::size_t length_size = Format::length_size;
	using LengthType = typename Format::LengthType;

	/*
	 * What make_msg() reserves when the caller gives no hint (§8.3.1).
	 *
	 * Not zero, and the reason is measured rather than aesthetic: a message
	 * built field by field from a capacity of zero walks the whole geometric
	 * ladder, which on the heap policy is 14 allocations and 453 requested
	 * bytes for a 100-byte payload. From 32 the same payload takes four
	 * (32 -> 48 -> 72 -> 108), and a typical short frame takes exactly one.
	 * An API that only performs well when the caller thought to pass a hint is
	 * a trap with good documentation.
	 *
	 * It costs nothing on a single-slab policy, which reports tx_max_size
	 * whatever it was asked for. A caller who wants no reserve at all says
	 * make_msg(0) — a zero capacity REQUEST, not an empty-only message: it
	 * can be written into and grown like any other.
	 *
	 * Clamped, because a policy may declare a limit below this default, and
	 * make_msg() must never fail merely because of its own default.
	 */
	static constexpr std::size_t default_capacity_hint =
		(max_send_size < 32u) ? max_send_size : 32u;

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
			m_allocator.deallocate_tx(m_activeTx.memory, m_activeTx.capacity);
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
	 * A new, EMPTY message to build into. Named make_ rather than get_ because
	 * it constructs an owning object and may allocate; there is nothing to
	 * "get".
	 *
	 *     auto msg = cobs.make_msg();
	 *     if (!msg ||
	 *         !msg.write<uint16_t>(id) ||
	 *         !msg.write_bytes(body)) {
	 *         return;               // exhausted, or the payload will not fit
	 *     }
	 *     const SendResult result = cobs.push(msg);
	 *
	 * Every write result has to be acted on. A failed write leaves the message
	 * intact and still usable, but also INCOMPLETE — pushing it anyway sends a
	 * truncated frame, which is memory-safe and protocol-nonsense. That is why
	 * they are [[nodiscard]], and why this example does not quietly cast the
	 * results away.
	 *
	 * The argument is a capacity hint, NOT an initial size: size() starts at
	 * zero either way, and the hint only spares growths for a caller who knows
	 * roughly how much is coming. Capacity then grows geometrically as the
	 * message is written (§8.3.1), so a caller who cannot predict the length
	 * does not have to.
	 *
	 *     make_msg()     default_capacity_hint, a practical reserve
	 *     make_msg(0)    a zero capacity REQUEST; pushed straight away it
	 *                    sends the canonical empty frame, but it may still
	 *                    be written into and grown like any other message
	 *     make_msg(N)    the caller knows a useful number
	 *
	 * An EMPTY result is the back-pressure signal that the configured TX
	 * storage is exhausted, and callers must handle it; CobsMsg is already a
	 * nullable owner, so there is nothing for an optional to add.
	 */
	[[nodiscard]] Msg make_msg() noexcept { return make_msg(default_capacity_hint); }

	[[nodiscard]] Msg make_msg(const std::size_t capacity_hint) noexcept
	{
		if (capacity_hint > max_send_size) {
			return {};
		}
		return Msg{m_allocator, capacity_hint};
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
			// The capacity the policy reported travels with the pointer, so a
			// policy that segregates by size class knows where the block
			// belongs without searching (§9.1).
			m_allocator.deallocate_tx(m_activeTx.memory, m_activeTx.capacity);
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
