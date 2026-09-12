/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "adapters/freertos/FreeRtosWake.h"

extern "C" TickType_t wake_default_ticks() noexcept
{ return uart::detail::wake_ticks_for(50u); }

extern "C" TickType_t wake_variable_ticks(uint32_t milliseconds) noexcept
{ return uart::detail::wake_ticks_for(milliseconds); }
