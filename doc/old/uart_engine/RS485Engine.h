/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * RS485Engine.h
 *
 *  Created on: May 21, 2025
 *      Author: admin
 */

#ifndef STM32_TOOLS_UART_ENGINE_RS485ENGINE_H_
#define STM32_TOOLS_UART_ENGINE_RS485ENGINE_H_

#include "UartEngine.h"

#if UART_ENGINE_ON && UART_ENGINE_RS485_ON && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))
#include "io/io.h"

template <UartType mode, reg BufferSize = 128>
class RS485Engine final : public UartEngine<mode, BufferSize>
{
public:
	RS485Engine() = default;

	using Base = UartEngine<mode, BufferSize>;

	struct Init {
		UART_HandleTypeDef* huart;  // UART descriptor
		gpio::Output de;
	};

	// Initializes the handler by storing the UART descriptor and starting DMA reception.
	bool init(const Init&);
	// Processes buffered data and restarts reception if needed.
	inline bool proceed(const u32 time);
	inline bool send(const u8* const data, const u16 size);

private:
	virtual inline void onTxCplt() noexcept final override;
	virtual inline void onErrorEvent() noexcept final override;

private:
	/*
	 * rs485 data en
	 */
	gpio::Output m_de;
};

template<UartType mode, reg BufferSize>
bool RS485Engine<mode, BufferSize>::init(const Init& settings)
{
	const bool base_init = Base::init({settings.huart});
	m_de = settings.de;
	m_de.write(false);
	return base_init;
}

template<UartType mode, reg BufferSize>
inline bool RS485Engine<mode, BufferSize>::send(const u8 *const data, const u16 size)
{
	if(Base::isTxBusy()) {
		return false;
	}

	m_de.write(true);
	{ __NOP();__NOP();__NOP(); }
	if (!Base::send(data,size)) {
		m_de.write(false);
		return false;
	}
	return true;
}

template<UartType mode, reg BufferSize>
inline void RS485Engine<mode, BufferSize>::onTxCplt() noexcept
{
	Base::onTxCplt();
	m_de.write(Base::m_isTxBusy);
}

template<UartType mode, reg BufferSize>
inline bool RS485Engine<mode, BufferSize>::proceed(const u32 time)
{
	if(Base::proceed(time)) {
		m_de.write(Base::m_isTxBusy);
		return true;
	}

	return false;
}

template<UartType mode, reg BufferSize>
inline void RS485Engine<mode, BufferSize>::onErrorEvent() noexcept
{
	Base::onErrorEvent();
	m_de.write(Base::m_isTxBusy);
}
#else
#	warning "[UART ENGINE]: RS485 ENGINE is disabled"
#endif /* UART_ENGINE_ON && UART_ENGINE_RS485_ON && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED)) */
#endif /* STM32_TOOLS_UART_ENGINE_RS485ENGINE_H_ */
