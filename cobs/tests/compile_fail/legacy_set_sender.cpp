/*
 * Split transport setters are physically absent. Sender and BusyQuery are
 * installed transactionally through bind(sender, busy).
 */
#include "Cobs.h"

void split_transport_pair(cobs::Endpoint<>& endpoint)
{
	endpoint.set_sender({});
}
