<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# UART Engine (STM32) — коротка документація

Універсальна легка бібліотека для прийому/передачі по UART/USART на STM32 з підтримкою IT, DMA та DMA Circular (ReceiveToIdle), внутрішньою буферизацією і колбеками. Додатково є обгортка для RS-485 з автоматичним керуванням лінією DE.

Основна ідея взята звідси https://github.com/STMicroelectronics/STM32CubeG4/tree/master/Projects/NUCLEO-G474RE/Examples/UART/UART_ReceptionToIdle_CircularDMA і базується на функції `HAL_UARTEx_ReceiveToIdle` яка при прийомі викликає колбек `void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)`

1) Для режиму IT треба просто включити глобальні переривання для uart
2) Для режиму DMA треба налаштувати dma_rx, dma_tx але не в циклічному режимі
3) Для режиму DMA Circular треба налаштувати dma_rx - циклічний режим, dma_tx - звичайний режим

- DMA Circular режим дозволяє комунікувати на швидкості більше ніж 1 Mbit з мінімальним оверхедом а якшо ще й ввімкнути периферійну fifo на uart - максимальна швидкість - як по даташиту
- Режим IT - самий простий режим для: `запустив і працює`
- Звичайний режим DMA - зроблений чисто щоб був на всякий випадок

### ** Саме головне і особливе в цій бібліотеці є то що вона викликає преривання і копіює в буфер лише в 2 випадках:**
1) Коли приходить сигнал IDLE з периферії UART (дані не передаються тобто на шині 1.5 біта перерив між фреймами)
2) 
   - Коли місце в буфері закінчилось і дані треба негайно забирати (у всіх 3 режимах IT, DMA та DMA Circular)
   - Половина буфера заповнена (у 2 режимах DMA та DMA Circular)

> Тобто ніяких переривань на кожен байт не відбувається, а саме переривання відбувається на цілий пакет!!!! Тому виклик функції `std::memcpy` цілком виправданий бо вона копіює ***не байтово!!!*** а максимально ефективно наскільки дозволяє залізо (навіть можливо що 128 байтними кусками)


## Можливості

* Прийом у режимах: `IT`, `DMA`, `DMA_CIRCULAR` (ReceiveToIdle).
* Два рівні обробки Rx:

  * миттєва (IT-handler) для швидких пакетів;
  * буферизована (loop-handler) через кільцевий буфер.
* Автовідновлення приймача при збої DMA/ORE/FE/NE/PE.
* Відправка через IT або DMA з подією завершення.
* Сумісність як з новими (ISR/RDR), так і зі старими (SR/DR) серіями STM32.
* RS-485: автоматичне керування `DE` під час передачі.

## Залежності та вимоги

* HAL UART/USART увімкнений у проєкті.
* Для режимів DMA потрібні `hdmarx` і `hdmatx` в `UART_HandleTypeDef`.
* Заголовки бібліотеки: `UartEngine.h`, `UartBase.h`, `UartType.h`, `uart_regs.h`, `uart_config.h`, опційно `RS485Engine.h`.
* Кільцевий буфер: `buffers::sbuf::ArrayRB` (вхідний кільцевик).
* Опційні утиліти: `irq/IRQGuard.h`, `io/io.h` (для RS-485).

## Увімкнення та конфігурація

Створи у своєму проєкті файл `uart_settings.h` і задай прапори:

```c
// uart_settings.h
#define UART_ENGINE_ON 1
#define UART_ENGINE_RS485_ON 1            // якщо потрібен RS-485
#define UART_ENGINE_INTERNAL_CALLBACKS_ON 1 // використати внутрішні HAL callbacks
```

Потім підключи конфіг:

```c++
#include "uart_config.h"   // цей хедер сам підтягне uart_settings.h, якщо він є
```

> Якщо `UART_ENGINE_INTERNAL_CALLBACKS_ON == 0`, ти маєш сам реалізувати HAL-колбеки і делегувати їх у бібліотеку (див. нижче).

## Типи і режими

Зручно користуватись псевдонімами:

```c++
#include "UartType.h"  // створює UartIT, UartDma, UartDmaCirc і RS485* при ввімкненому RS485
```

Або напряму:

```c++
UartEngine<UartType::UART_DMA, 256> uart;   // BufferSize ≥ 32 і кратний слову
```

## Швидкий старт (UART DMA)

```c++
#include "UartType.h"

// HAL handle з CubeMX
extern UART_HandleTypeDef huart3;

static UartDma uart;     // або UartIT / UartDmaCirc

void app_uart_init() {
    // 1) Ініціалізація
    uart.init({ .huart = &huart3 });

    // 2) Підписки на події (вибрати якусь 1 або loop або IT)
    uart.subscribeRxIt([](const u8* data, u16 size) -> bool { //<-- для IT без додаткового копіювання
        // Fast-path parse; return false to save into loop-buffer
        // return true  -> "consumed", НЕ зберігати у буфер
        // return false -> зберегти копію у внутрішній буфер
        return false;
    });

    uart.subscribeRxLoop([](const u8* data, u16 size, u32 time_ms) -> u16 { //<-- для loop дані копіюються в кільцевий буфер
        // Process chunk and tell how many bytes were consumed
        // Safe to keep: library will pop 'consumed' bytes
        return size;
    });

    uart.subscribeTx([](status_t st) {
        // Notify TX completion or fail
        (void)st;
    });

    uart.subscribeError([](status_t st) {
        // UART/DMA error happened; already auto-restarted if needed
        (void)st;
    });
}

void app_loop(u32 now_ms) {
    uart.proceed(now_ms); // перевірка стану + вивантаження буфера
}

// Відправка
void app_send(const u8* buf, u16 n) {
    // DMA використовується для великих пакетів, IT — для малих (всередині бібліотеки)
    (void)uart.send(buf, n);
}
```

## RS-485 приклад

```c++
#include "UartType.h"
#include "io/io.h"   // gpio::Output

extern UART_HandleTypeDef huart1;

// Active-high DE pin
static RS485Dma rs485;

void app_rs485_init() {
    gpio::Output de{/*port=*/GPIOA, /*pin=*/GPIO_PIN_8};
    rs485.init({ .huart = &huart1, .de = de });
}

void rs485_send(const u8* data, u16 len) {
    // Library toggles DE automatically, with small guard NOPs before TX
    rs485.send(data, len);
}
```

## Колбеки HAL

### Варіант A: внутрішні (простіше)

Встанови `#define UART_ENGINE_INTERNAL_CALLBACKS_ON 1` у `uart_settings.h`.
Бібліотека надає визначення:

* `HAL_UARTEx_RxEventCallback`
* `HAL_UART_TxCpltCallback`
* `HAL_UART_ErrorCallback`

і сама делегує їх у потрібний інстанс.

### Варіант B: власні

Якщо хочеш контролювати все сам, зроби `UART_ENGINE_INTERNAL_CALLBACKS_ON 0` і додай у свій файл:

```c
extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
    UartBase::rxCpltHandler(huart, Size);
}
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    UartBase::txCpltHandler(huart);
}
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    UartBase::errorHandler(huart);
}
```

## Поведінка `proceed(time_ms)`

* Раз на ~200 мс перевіряє стан HAL UART/DMA.
* При зависанні, помилці DMA або стані `ERROR/TIMEOUT` перезапускає прийом.
* Вивантажує накопичені дані з внутрішнього кільцевика в твій `rx_loop_handler`.

## Обмеження і нотатки

* `BufferSize ≥ 32` і кратний розміру слова; буфер вирівняний на 32 байти для DMA.
* Для DMA відправки малі пакети можуть піти через IT, великі — DMA (всередині є поріг ~16 байт).
* Для `DMA_CIRCULAR` бібліотека обробляє wrap-around коректно.
* Для RS-485 лінія `DE` активується перед `TX` і звільняється після завершення передачі.

## Підказки по інтеграції

* Можеш викликати `UartBase::reserve(N)` на старті, якщо очікується до `N` екземплярів.
* Якщо використовуєш кілька UART одночасно, просто створюй кілька інстансів — реєстр об’єктів ведеться автоматично.
* На старих серіях STM32 з регістрами `SR/DR` обробка помилок/скидання прапорів виконується так само прозоро.

## Ліцензія та авторство

* Автор: ти й так знаєш хто.
* Ліцензія: додай свою або напиши тут.

---

Готово. Коротко, без води, і з прикладами, які не демонструють відчай. Якщо захочеш докинути схему підключення DE або таблицю станів, просто скажи куди вставити — я зроблю.
