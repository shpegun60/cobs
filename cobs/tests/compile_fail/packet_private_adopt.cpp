/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Only detail::Receiver may transfer a ready-queue reference into Packet.
 */
#include "Cobs.h"

using Packet = cobs::Endpoint<>::Packet;

Packet forged_packet = Packet::adopt(nullptr);
