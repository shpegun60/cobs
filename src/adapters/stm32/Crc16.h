/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef CRC_STM32_CRC16_H_
#define CRC_STM32_CRC16_H_

#include "main.h" // target HAL/CMSIS; HAL_CRC_MODULE_ENABLED required
#include "crc/Crc.h"
#include <bit>
#include <cstring>

#if !defined(HAL_CRC_MODULE_ENABLED) || !defined(CRC_POLYLENGTH_16B)
#error "stm32::Crc16 needs an enabled, programmable STM32 CRC peripheral"
#endif

namespace crc::stm32 {

// A user-policy example for the programmable STM32 CRC IP, tested on H7S3.
// The handle and enabled peripheral clock must outlive every calculate().
// Constructor only retains the handle, so static construction before MX_CRC_Init
// is safe. No hardware access happens until calculate().
//
// Exclusive use for the duration of calculate() is the caller's responsibility:
// no other task/ISR/DMA may touch this CRC instance then. No hidden lock, IRQ
// masking, heap allocation or HAL tick. Each call resets AND programs its own
// CRC16/MODBUS configuration; sequential policies may safely reuse the instance.
// It does not update HAL's cached Init fields or preserve a preceding calculation.
// Input must be ordinary CPU-readable memory (not a device/MMIO byte stream).
class Crc16 final : public crc::Codec<uint16_t> {
public:
    explicit Crc16(CRC_HandleTypeDef& handle) noexcept : handle_(&handle) {}

    [[nodiscard]] uint16_t calculate(std::span<const uint8_t> bytes) const noexcept
    {
        auto& registers = *handle_->Instance;
        // Stop/reset the preceding calculation before changing polynomial.
        // Hardware uses the normal polynomial; software's 0xA001 is reflected.
        registers.CR = CRC_CR_RESET;
        registers.POL = 0x8005u;
        registers.INIT = 0xFFFFu;
        registers.CR = CRC_POLYLENGTH_16B | CRC_INPUTDATA_INVERSION_BYTE |
                       CRC_OUTPUTDATA_INVERSION_ENABLE | CRC_CR_RESET;
        const uint8_t* input = bytes.data();
        std::size_t left = bytes.size();
        while (left >= 4u) {
            uint32_t word;
            // memcpy admits any input alignment without aliasing a byte buffer
            // as uint32_t. The target compiler chooses legal loads; on M7 this
            // is normally LDR + REV, or byte loads with -mno-unaligned-access.
            std::memcpy(&word, input, sizeof(word));
            if constexpr (std::endian::native == std::endian::little) {
                word = __REV(word);
            }
            registers.DR = word; // first stream byte enters the highest lane
            input += 4u;
            left -= 4u;
        }
        // Character access may alias the register's declared uint32_t type;
        // a type-punned uint16_t store would violate C++ strict aliasing.
        while (left != 0u) {
            *reinterpret_cast<volatile uint8_t*>(&registers.DR) = *input++;
            --left;
        }
        // This IP reverses within the selected polynomial width: CRC16 is
        // returned in the low 16 bits, including when REV_OUT is enabled.
        return static_cast<uint16_t>(registers.DR);
    }

private:
    CRC_HandleTypeDef* handle_;
};

static_assert(crc::Policy<Crc16> && Crc16::wire_size == 2u);
static_assert(sizeof(Crc16) == sizeof(void*));

} // namespace crc::stm32
#endif
