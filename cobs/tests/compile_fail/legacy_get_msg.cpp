/*
 * The pre-refactor message factory name is physically absent; no alias remains.
 */
#include "Cobs.h"

void use_removed_name(cobs::Endpoint<>& endpoint)
{
	auto message = endpoint.get_msg();
	(void)message;
}
