/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Public COBS names for the transport-neutral, stateless payload readers. */

#ifndef COBS_READ_H_
#define COBS_READ_H_

#include "../wire/Read.h"

namespace cobs {

using wire::read_native;
using wire::read_be;
using wire::read_le;
using wire::read_bytes;

} // namespace cobs

#endif /* COBS_READ_H_ */
