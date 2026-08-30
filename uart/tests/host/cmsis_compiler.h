/*
 * Host stand-in for the CMSIS compiler header: a PRIMASK that actually
 * behaves like one. Interrupts raised while PRIMASK is set do NOT run — they
 * become pending and are dispatched when PRIMASK is restored to 0, exactly as
 * a Cortex-M does. Without this the harness would test a parallel universe in
 * which an ISR magically executes inside __disable_irq().
 */
#ifndef FAKE_CMSIS_COMPILER_H_
#define FAKE_CMSIS_COMPILER_H_

#include <cstdint>

namespace fake {

void dispatch_pending() noexcept; // defined in fake_hal.cpp

extern uint32_t g_primask;

} // namespace fake

inline uint32_t __get_PRIMASK() noexcept { return fake::g_primask; }
inline void __disable_irq() noexcept { fake::g_primask = 1u; }
inline void __enable_irq() noexcept { fake::g_primask = 0u; fake::dispatch_pending(); }

inline void __set_PRIMASK(const uint32_t v) noexcept
{
	fake::g_primask = v;
	if (v == 0u) {
		fake::dispatch_pending(); // leaving the critical section runs what queued up
	}
}

#define __DMB() ((void)0)
#define __DSB() ((void)0)
#define __ISB() ((void)0)

#endif /* FAKE_CMSIS_COMPILER_H_ */
