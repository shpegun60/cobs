/**
* @file IRQGuard.h
* @brief A class for managing interrupt state using RAII.
*
* This file contains the definition of the IRQGuard class, which provides automatic
* locking and unlocking of hardware interrupts in its constructor
* and destructor respectively, using the RAII approach.
*
* @author Shpegun60
* @date February 14, 2025
*/

#ifndef IRQ_GUARD_H_
#define IRQ_GUARD_H_

#include "cmsis_compiler.h" // Header file providing __get_PRIMASK(), __set_PRIMASK(), __disable_irq()
#include "macro.h"
/**
 * @brief RAII-based guard for managing IRQ state using PRIMASK.
 *
 * The IRQGuard class automatically disables interrupts when an object is created
 * and restores their state when the object is destroyed. It uses the PRIMASK register
 * to save and restore the interrupt state, ensuring that interrupts are only
 * re-enabled to their original state.
 */
class IRQGuard
{
    // Copy constructor and assignment operator are deleted to prevent multiple instances
    // from interfering with the IRQ state management.
	_DELETE_COPY_MOVE(IRQGuard);
public:
    /**
     * @brief Constructor.
     *
     * Saves the current state of the PRIMASK register and disables interrupts.
     */
    inline IRQGuard() noexcept : primask_(__get_PRIMASK()) {
        __disable_irq();            // Disable interrupts
        __DMB();
    }

    /**
     * @brief Destructor.
     *
     * Restores the PRIMASK register to its original state, re-enabling interrupts
     * only if they were enabled before the object was created.
     */
    inline ~IRQGuard() {
        __DMB();
        __set_PRIMASK(primask_);     // Restore the original PRIMASK state
    }

private:
    uint32_t primask_;  // Variable to store the original PRIMASK state
};

#endif /* IRQ_GUARD_H_ */
