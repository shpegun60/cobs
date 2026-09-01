/*
 * TxAllocation — what an allocator policy hands back from allocate_tx().
 *
 * Contract: doc/COBS_ENGINE.md §9.1. Two fields, both in PAYLOAD units:
 *
 *      memory      the block, or null if the request could not be met
 *      capacity    how many payload bytes this particular block permits
 *
 * `capacity` is not "how much the allocator happened to give physically" —
 * that was the descriptor this contract deliberately removed. It is the one
 * fact a caller cannot derive: a single-slab pool honours a seven-byte request
 * out of a block that holds tx_max_size, and refusing to say so would leave
 * paid-for memory unusable. Physical block size, alignment and padding stay
 * the policy's private business.
 *
 * Obligations on a non-null allocation:
 *
 *      requested <= capacity <= tx_max_size
 *      the block holds at least
 *          CobsFrameFormat<...>::tx_storage_size_for_capacity(capacity) bytes
 *
 * That last one is HEADER-INCLUSIVE and this is the place people will read it
 * from, so it is worth being exact. What gets COBS-encoded is the decoded
 * frame — [length][payload] — not the payload alone, so the requirement is
 *
 *      cobs::codec::max_wire_size(length_size + capacity)
 *
 * and a policy sized with cobs::codec::max_wire_size(capacity) is one or two bytes
 * short of every block it hands out. Ask the format rather than open-coding
 * it: an earlier revision of this comment stated the payload-only formula,
 * which was correct before the length prefix existed and was, afterwards,
 * a set of instructions for writing an allocator that CobsMsg::encode()
 * overruns.
 */

#ifndef COBS_TX_ALLOCATION_H_
#define COBS_TX_ALLOCATION_H_

#include <cstddef>

struct TxAllocation {
	std::byte*  memory   = nullptr;
	std::size_t capacity = 0;
};

#endif /* COBS_TX_ALLOCATION_H_ */
