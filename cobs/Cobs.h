/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Endpoint — the assembled engine. RX vertical, TX vertical, one storage strategy.
 *
 * Contract: doc/ARCHITECTURE.md. Wire details are in doc/PROTOCOL.md and the
 * memory extension boundary is in doc/STORAGE.md. Everything difficult
 * already lives one layer down; what is here is the transport handshake and
 * the single active transfer.
 *
 *     RX:  bytes -> cobs::codec::Decoder -> storage -> RxBlock -> Packet
 *     TX:  Message -> encoder -> sender delegate -> activeTx -> storage
 *
 * ---------------------------------------------------------------------------
 * LIFETIME PRECONDITIONS. Not decoration — violating either is a use-after-
 * free that no amount of internal bookkeeping can repair:
 *
 *  1. An Endpoint object must OUTLIVE every Packet and every Message it handed
 *     out. Both hold a pointer to the storage living inside this object, and
 *     both call back into it when they release. That is also why Endpoint is
 *     neither copyable nor movable: moving it would turn every outstanding
 *     packet's owner pointer into a souvenir of a previous life.
 *
 *  2. An Endpoint object must not be destroyed while a transfer is in flight
 *     (`tx_active()` true and the transport still busy). The transport is
 *     physically reading a block that is about to stop existing. Check
 *     `tx_active()` and drain with `poll()` before letting it go.
 * ---------------------------------------------------------------------------
 * TRANSPORT CALLBACK PRECONDITIONS. Sender and busy-query targets execute
 * synchronously inside noexcept Endpoint methods. They must not throw and
 * must not re-enter bind(), unbind(), send(), or poll() on this same Endpoint.
 * A sender returning true has borrowed the exact span until busy() becomes
 * false; returning false means it borrowed nothing. busy() itself is a
 * side-effect-free lifetime query and may return false only after the
 * transport has stopped touching the bytes.
 * ---------------------------------------------------------------------------
 */

#ifndef COBS_H_
#define COBS_H_

#include "Format.h"
#include "Stats.h"
#include "Storage.h"
#include "detail/Message.h"
#include "detail/Packet.h"
#include "detail/Receiver.h"

#include "tiny_delegate.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace cobs {

enum class SendResult : uint8_t {
	Sent,     // the transport accepted the frame; Endpoint now holds the block
	Busy,     // a transfer is already in flight; the message is untouched
	Unbound,  // no sender / busy-query delegate pair has been bound
	Failed,   // the transport refused to start; the message stays Encoded
	Invalid,  // the message owns no block, or belongs to another engine
};

template<class StorageT = cobs::Heap<>>
class Endpoint final {
	static_assert(cobs::Storage<StorageT>,
		"Endpoint storage must satisfy the cobs::Storage contract");

public:
	using StorageType = StorageT;
	using Message = cobs::Message<StorageT>;
	using Packet = cobs::Packet<StorageT>;
	using Format = typename StorageT::Format;

	/*
	 * Republished so callers have one place to ask, and so a change of storage
	 * is visible rather than implied (§4.1). Both are BODY limits — the
	 * largest application payload this instance will receive and send. The
	 * decoded frame is length_size bytes longer than either, which is why
	 * neither is called max_decoded_size any more: that name was one header
	 * away from the truth, on a layer where being one header out is the
	 * easiest mistake there is.
	 */
	static constexpr std::size_t max_receive_size = Format::max_receive_size;
	static constexpr std::size_t max_send_size    = Format::max_send_size;

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
	static constexpr std::size_t length_size = Format::length_size;
	using LengthType = typename Format::LengthType;

	/*
	 * What make_message() reserves when the caller gives no hint (§8.3.1).
	 *
	 * Not zero, and the reason is measured rather than aesthetic: a message
	 * built field by field from a capacity of zero walks the whole geometric
	 * ladder, which on Heap is 14 allocations and 453 requested
	 * bytes for a 100-byte payload. From 32 the same payload takes four
	 * (32 -> 48 -> 72 -> 108), and a typical short frame takes exactly one.
	 * An API that only performs well when the caller thought to pass a hint is
	 * a trap with good documentation.
	 *
	 * It costs nothing on Pool, which reports max_send_size
	 * whatever it was asked for. A caller who wants no reserve at all says
	 * make_message(0) — a zero capacity REQUEST, not an empty-only message: it
	 * can accept appends and grow like any other.
	 *
	 * Clamped, because a Format may declare a limit below this default, and
	 * make_message() must never fail merely because of its own default.
	 */
	static constexpr std::size_t default_capacity_hint =
		(max_send_size < 32u) ? max_send_size : 32u;

	using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
	using BusyQuery = tiny::delegate<bool()>;

	Endpoint() noexcept = default;

	// For storage that needs runtime arguments — an external memory region, a
	// handle to somebody else's arena. It is constructed in place from
	// them, so it need not be copyable or movable (§9.4). There is
	// deliberately no constructor taking a ready-made instance: it would only
	// serve copyable ones, and one way in is enough.
	template<class... Args>
	explicit Endpoint(std::in_place_t, Args&&... args) noexcept
		: m_storage(std::forward<Args>(args)...) {}

	Endpoint(const Endpoint&) = delete;
	Endpoint& operator=(const Endpoint&) = delete;
	Endpoint(Endpoint&&) = delete;
	Endpoint& operator=(Endpoint&&) = delete;

	~Endpoint()
	{
		// Precondition 2 above says this must not happen with the transport
		// still reading. If a block is here at all, the pool is about to be
		// destroyed with it, so returning it only keeps the pool's own
		// accounting honest for anything watching.
		if (m_activeTx.memory != nullptr) {
			m_storage.release_tx(m_activeTx);
			m_activeTx = {};
		}
	}

	/* ------------------------------- setup ------------------------------- */

	/*
	 * Both delegates, together, or neither. Two independent setters made a
	 * genuine DMA mine reachable while the link was IDLE, so the previous
	 * "no rebinding during a transfer" guard was necessary but not enough:
	 *
	 *     set_sender(uartB);            // and the busy-query setter is skipped,
	 *                                   // fails, or simply comes later
	 *     -> sender = B, busy query = A
	 *     send()    starts DMA on B
	 *     poll() asks A, which is idle
	 *     -> the block is freed while B is still reading it
	 *
	 * A pairing this important is not something to document and hope for.
	 * Taking both at once makes the mixed states unrepresentable, and the
	 * half-bound ones too: a sender with an empty busy query is refused.
	 *
	 * Empty delegates are refused: removing a transport is the separate,
	 * explicit unbind() operation below. Both operations are refused while a
	 * transfer is in flight — the old transport is still reading the active
	 * block, and a new busy query answering "idle" would have poll() free it
	 * out from under the DMA.
	 */
	[[nodiscard]] bool bind(Sender sender, BusyQuery busy) noexcept
	{
		if (m_activeTx.memory != nullptr) {
			return false;
		}
		return m_transport.bind(
			static_cast<Sender&&>(sender), static_cast<BusyQuery&&>(busy));
	}

	[[nodiscard]] bool unbind() noexcept
	{
		if (m_activeTx.memory != nullptr) {
			return false;
		}
		m_transport.unbind();
		return true;
	}

	/* --------------------------------- RX -------------------------------- */

	void consume(std::span<const uint8_t> bytes) noexcept { m_rx.consume(bytes); }
	void notify_gap() noexcept { m_rx.gap(); }

	[[nodiscard]] Packet pop_packet() noexcept { return m_rx.pop_packet(); }
	[[nodiscard]] bool has_packet() const noexcept { return m_rx.has_packet(); }

	/* --------------------------------- TX -------------------------------- */

	/*
	 * A new, EMPTY message to build into. Named make_ rather than get_ because
	 * it constructs an owning object and may allocate; there is nothing to
	 * "get".
	 *
	 *     auto msg = cobs.make_message();
	 *     if (!msg ||
	 *         !msg.append_native<uint16_t>(id) ||
	 *         !msg.append_bytes(body)) {
	 *         return;               // exhausted, or the payload will not fit
	 *     }
	 *     const SendResult result = cobs.send(msg);
	 *
	 * Every append result has to be acted on. A failed append leaves the message
	 * intact and still usable, but also INCOMPLETE — sending it anyway transmits a
	 * truncated frame, which is memory-safe and protocol-nonsense. That is why
	 * they are [[nodiscard]], and why this example does not quietly cast the
	 * results away.
	 *
	 * The argument is a capacity hint, NOT an initial size: size() starts at
	 * zero either way, and the hint only spares growths for a caller who knows
	 * roughly how much is coming. Capacity then grows geometrically as the
	 * message is built (§8.3.1), so a caller who cannot predict the length
	 * does not have to.
	 *
	 *     make_message()     default_capacity_hint, a practical reserve
	 *     make_message(0)    a zero capacity REQUEST; sent straight away it
	 *                    sends the canonical empty frame, but it may still
	 *                    accept appends and grow like any other message
	 *     make_message(N)    the caller knows a useful number
	 *
	 * An EMPTY result is the back-pressure signal that the configured TX
	 * storage is exhausted, and callers must handle it; Message is already a
	 * nullable owner, so there is nothing for an optional to add.
	 */
	[[nodiscard]] Message make_message() noexcept { return make_message(default_capacity_hint); }

	[[nodiscard]] Message make_message(const std::size_t capacity_hint) noexcept
	{
		if (capacity_hint > max_send_size) {
			return {};
		}
		return Message{m_storage, capacity_hint};
	}

	/*
	 * Takes the message by REFERENCE, not by value or rvalue, because on
	 * every outcome except Sent the caller keeps it and retries (§8.1):
	 *
	 *   Busy   -> still Building, raw payload untouched and still writable
	 *   Failed  -> still Encoded, and send() may retry the SAME wire frame
	 *   Sent   -> the block moved here; the message is Empty again
	 */
	[[nodiscard]] SendResult send(Message& msg) noexcept
	{
		if (!msg || !msg.belongs_to(m_storage)) {
			return SendResult::Invalid;
		}
		if (!m_transport.bound()) {
			return SendResult::Unbound;
		}
		if (m_activeTx.memory != nullptr || m_transport.busy()) {
			++m_txStats.send_refused_busy;
			return SendResult::Busy;
		}

		const auto wire = msg.encode();
		if (wire.empty()) {
			return SendResult::Invalid;
		}
		if (!m_transport.start(wire)) {
			++m_txStats.send_failed;
			return SendResult::Failed; // message stays Encoded, frame retryable
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
	void poll() noexcept
	{
		if (m_activeTx.memory != nullptr && !m_transport.busy()) {
			// The capacity storage reported travels with the pointer, so a
			// strategy that segregates by size class knows where the block
			// belongs without searching (§9.1).
			m_storage.release_tx(m_activeTx);
			m_activeTx = {};
		}
	}

	/* ----------------------------- observation --------------------------- */

	// A value snapshot deliberately: callers see RX and TX together but cannot
	// mutate counters or retain references into the two state machines that own
	// them. Storage-specific pool occupancy remains available through storage().
	[[nodiscard]] cobs::Stats stats() const noexcept
	{
		return cobs::Stats{m_rx.stats(), m_txStats};
	}
	// Const on purpose: statistics and geometry are worth reading, but a
	// mutable reference would let a caller allocate behind the engine's back
	// and hand out blocks it never learns about.
	[[nodiscard]] const StorageT& storage() const noexcept { return m_storage; }

private:
	/*
	 * One owner for the one transport invariant. It contains exactly the same
	 * two owning tiny::delegate objects as before; bound state is derived from
	 * them, so grouping the pair adds neither a flag nor a weaker lifetime
	 * model. A rejected bind validates both arguments before touching the live
	 * pair, making rebinding transactional.
	 */
	class Transport final {
	public:
		[[nodiscard]] bool bind(Sender sender, BusyQuery busy) noexcept
		{
			if (!sender || !busy) {
				return false;
			}
			m_sender = static_cast<Sender&&>(sender);
			m_busy = static_cast<BusyQuery&&>(busy);
			return true;
		}

		void unbind() noexcept
		{
			m_sender = nullptr;
			m_busy = nullptr;
		}

		[[nodiscard]] bool bound() const noexcept
		{
			return static_cast<bool>(m_sender) && static_cast<bool>(m_busy);
		}

		[[nodiscard]] bool busy() const noexcept { return m_busy(); }

		[[nodiscard]] bool start(const std::span<const uint8_t> frame) const noexcept
		{
			return m_sender(frame);
		}

	private:
		Sender m_sender{};
		BusyQuery m_busy{};
	};

	[[no_unique_address]] StorageT m_storage{};
	cobs::detail::Receiver<StorageT> m_rx{m_storage};

	Transport m_transport{};

	cobs::TxBlock m_activeTx{};
	cobs::Stats::Tx m_txStats{};
};

} // namespace cobs

#endif /* COBS_H_ */
