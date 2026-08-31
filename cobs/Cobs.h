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

	// For a policy that needs runtime arguments — an external memory region,
	// a handle to somebody else's arena. A policy that is itself a light
	// copyable handle can simply be passed; not every policy has to be
	// copyable or movable (§9.4).
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
		if (m_activeTx != nullptr) {
			m_allocator.deallocate_tx(m_activeTx);
			m_activeTx = nullptr;
		}
	}

	/* ------------------------------- setup ------------------------------- */

	// Refused while a transfer is in flight, and that is not fussiness: the
	// old transport is still reading the active block, and a new tx_busy()
	// answering "not busy" would have proceed() free it out from under the
	// DMA that is reading it.
	[[nodiscard]] bool set_sender(Sender sender) noexcept
	{
		if (m_activeTx != nullptr) {
			return false;
		}
		m_sender = static_cast<Sender&&>(sender);
		return true;
	}
	[[nodiscard]] bool set_tx_busy(TxBusy busy) noexcept
	{
		if (m_activeTx != nullptr) {
			return false;
		}
		m_txBusy = static_cast<TxBusy&&>(busy);
		return true;
	}

	/* --------------------------------- RX -------------------------------- */

	void consume(std::span<const uint8_t> bytes) noexcept { m_rx.consume(bytes); }
	void gap() noexcept { m_rx.gap(); }

	[[nodiscard]] Ref pop_packet() noexcept { return m_rx.pop_packet(); }
	[[nodiscard]] bool has_packet() const noexcept { return m_rx.has_packet(); }

	/* --------------------------------- TX -------------------------------- */

	// An empty message when the TX pool is dry — CobsMsg is already a nullable
	// owner, so there is nothing for an optional to add.
	[[nodiscard]] Msg get_msg() noexcept { return Msg{m_allocator}; }

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
		if (m_activeTx != nullptr || m_txBusy()) {
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

	[[nodiscard]] bool tx_active() const noexcept { return m_activeTx != nullptr; }

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
		if (m_activeTx != nullptr && !m_txBusy()) {
			m_allocator.deallocate_tx(m_activeTx);
			m_activeTx = nullptr;
		}
	}

	/* ----------------------------- observation --------------------------- */

	[[nodiscard]] const RxStats& rx_stats() const noexcept { return m_rx.stats(); }
	[[nodiscard]] const TxStats& tx_stats() const noexcept { return m_txStats; }
	[[nodiscard]] Allocator& allocator() noexcept { return m_allocator; }

private:
	[[no_unique_address]] Allocator m_allocator{};
	CobsRx<Allocator> m_rx{m_allocator};

	Sender m_sender{};
	TxBusy m_txBusy{};

	std::byte* m_activeTx = nullptr;
	TxStats m_txStats{};
};

#endif /* COBS_H_ */
