/*
 * Test-facing controls of the fake HAL: inject bus events, inject faults, and
 * read back the ownership model the fake DMA maintains.
 */
#ifndef FAKE_HAL_MODEL_H_
#define FAKE_HAL_MODEL_H_

#include "main.h"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fake {

// Where a physical RX buffer is in its lifecycle. The driver must keep these
// disjoint: whatever the DMA owns can never be what the consumer is reading.
enum class Slot { Free, DmaOwned, ConsumerVisible };

struct Model {
	UART_HandleTypeDef* huart = nullptr;

	/* --- bus state --- */
	bool     rx_armed = false;
	uint8_t* rx_dst   = nullptr;
	uint16_t rx_len   = 0;
	bool     tx_armed = false;
	const uint8_t* tx_src = nullptr;
	uint16_t tx_len   = 0;

	uint32_t tick = 0;

	/* --- fault injection --- */
	int  fail_abort_receive  = 0; // N next calls return HAL_TIMEOUT (transfer stays live)
	int  fail_abort_transmit = 0;
	int  fail_arm            = 0; // N next ReceiveToIdle_DMA calls fail
	bool rx_cplt_inside_abort = false; // ST: an abort may raise the completion callback
	bool tx_cplt_inside_abort = false;

	/* --- ownership bookkeeping, keyed by buffer address --- */
	std::map<const void*, Slot> slots;
	std::vector<const void*>    armed_history;

	/* --- observed events --- */
	std::vector<std::string> rx_data;   // payload handed to the RxHandler
	std::vector<std::string> rx_events; // "data:<n>" / "gap"
	std::vector<bool>        tx_results;
	std::vector<uint32_t>    errors;

	/* --- invariant violations (empty == healthy) --- */
	std::vector<std::string> violations;
};

Model& model() noexcept;
void   reset() noexcept;
void   fail(const std::string& what) noexcept;

/* Bus events, all raised through the pending-IRQ path so PRIMASK is honoured */
void rx_bytes(const void* data, std::size_t n) noexcept; // DMA writes into the armed buffer
void rx_idle() noexcept;                                 // partial transfer + IDLE
void rx_tc() noexcept;                                   // buffer filled
void rx_error(uint32_t code) noexcept;                   // blocking RX error (DMA mode)
void tx_done() noexcept;
void tx_error() noexcept;
void advance_tick(uint32_t ms) noexcept;

/* Marks used by the fixture's RxHandler to police ownership. */
void note_consumer_sees(const void* buf) noexcept;
void note_consumer_done(const void* buf) noexcept;

std::size_t distinct_chunks_armed() noexcept;

} // namespace fake

#endif /* FAKE_HAL_MODEL_H_ */
