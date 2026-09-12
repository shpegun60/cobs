<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Почни звідси: повернення до проєкту через місяць

<!-- toc -->

Contents

- [Що тут взагалі зроблено](#що-тут-взагалі-зроблено)
- [Обери готовий маршрут](#обери-готовий-маршрут)
- [Словник типів: що з чим не плутати](#словник-типів-що-з-чим-не-плутати)
- [Політики без магії](#політики-без-магії)
  - [Heap чи Pool](#heap-чи-pool)
  - [Як змінити розмір даних](#як-змінити-розмір-даних)
  - [Bitwise, Table, NoCrc, периферія](#bitwise-table-nocrc-периферія)
- [Однаковий цикл Message і Packet](#однаковий-цикл-message-і-packet)
  - [Метадані задаються один раз](#метадані-задаються-один-раз)
  - [Читання та запис полів](#читання-та-запис-полів)
  - [Що означає send result](#що-означає-send-result)
- [Адаптер, wake чи ручне підключення](#адаптер-wake-чи-ручне-підключення)
- [Де протоколи справді відрізняються](#де-протоколи-справді-відрізняються)
  - [RTU: цілий ADU проти потоку](#rtu-цілий-adu-проти-потоку)
  - [TCP: MBAP, а не таблиця функцій](#tcp-mbap-а-не-таблиця-функцій)
  - [COBS: delimiter і конфігурація обох боків](#cobs-delimiter-і-конфігурація-обох-боків)
- [Якщо нічого не працює](#якщо-нічого-не-працює)
- [Як переконатися, що приклад не застарів](#як-переконатися-що-приклад-не-застарів)

<!-- /toc -->

[Головна](../README.md) · [Уся документація](README.md) · [Каталог прикладів](EXAMPLES.md)

Це посібник користувача, не звіт про розробку. Щоб підключити бібліотеку,
не потрібно спочатку читати аудити, дизасемблер або історію рефакторингу.
Спочатку обери свій сценарій нижче, відкрий повний приклад і лише потім
змінюй потрібні параметри. Тут і далі `N` у `Format` — твої дані, а не розмір
буфера з усіма службовими байтами.

## Що тут взагалі зроблено

Є три протокольні endpoint-и та окремий UART-драйвер. Протокол не знає,
звідки прийшли байти; UART не знає, що таке твій пакет.

| Частина | Для чого | Куди дивитися |
|---|---|---|
| COBS | власні двійкові повідомлення, включно з нулями всередині; потік може різатися довільно | `src/cobs/`, [посібник](USER_GUIDE.md#cobs-quick-start) |
| Modbus RTU | адреса, функція, дані, CRC; за потреби складання відомих функцій із фрагментів | `src/modbus/rtu/`, [RTU](../src/modbus/README.md) |
| Modbus TCP | transaction ID, unit, функція, дані; межі завжди через MBAP | `src/modbus/tcp/`, [TCP](../src/modbus/tcp/README.md) |
| UART | DMA RX/TX, chunks, кеш, переривання, відновлення; жодного парсингу протоколів | `src/uart/`, [UART API](USER_GUIDE.md#stm32-uart-quick-start) |
| wire | спільні storage, читання/запис чисел, результат send | `src/wire/`, [storage](STORAGE.md) |
| crc | CRC8/16/32/64, Bitwise/Table, NoCrc, контракт власної реалізації | `src/crc/`, [CRC](../src/crc/README.md) |
| adapters | готове з'єднання протоколу з UART/Qt, wake для FreeRTOS, STM32 CRC | `src/adapters/`, [інтеграція](INTEGRATION.md) |

`app/` — окремий Qt GUI цього репозиторію. Копіювати його для використання
бібліотеки не треба. `libs/` — залежності; `detail/` усередині модулів —
нутрощі, на які користувацький код не має спиратися. `tests/` — перевірки,
не обов'язкові файли прошивки. `doc/examples/` — саме приклади застосування.

## Обери готовий маршрут

| Хочу… | Відкрити |
|---|---|
| спочатку побачити працюючий обмін без заліза | [protocols.cpp](examples/protocols.cpp), запуск `sh doc/examples/build.sh` |
| STM32 + COBS, звичайний головний цикл | [cobs_adapter.cpp](examples/cobs_adapter.cpp), [пояснення](INTEGRATION.md#stm32-with-an-adapter) |
| STM32 + RTU, запити приходять із UART | [rtu_adapter.cpp](examples/rtu_adapter.cpp): framed Request endpoint, відповідь зберігається при Busy |
| COBS/RTU в окремій задачі FreeRTOS | [freertos_entry.cpp](examples/freertos_entry.cpp), [повний маршрут](FREERTOS.md#complete-communication-task) |
| лише UART + wake, без протоколу | [uart_wake.cpp](examples/uart_wake.cpp), [без адаптера](FREERTOS.md#raw-uart-without-a-protocol-adapter) |
| COBS вручну, без UartAdapter | [cobs_direct.cpp](examples/cobs_direct.cpp); `DOC_WAKE` додає сон задачі |
| RTU без адаптера | [rtu_direct.cpp](examples/rtu_direct.cpp), спершу прочитай обмеження цілих ADU нижче |
| Qt + QSerialPort + COBS | [Qt COBS](QT.md#cobs-over-qserialport) |
| Qt + Modbus RTU з чергою запитів і timeout/retry | [Qt RTU client](QT.md#rtu-client-and-server) |
| Qt TCP з MBAP, без RTU-фреймера | [Qt TCP](QT.md#tcp-over-qtcpsocket) |
| інший транспорт: USB, власний драйвер, радіоканал | [контракт Sender/BusyQuery](INTEGRATION.md#your-own-byte-transport) |
| власна пам'ять або калькулятор | [policies.cpp](examples/policies.cpp), [storage](STORAGE.md), [CRC](../src/crc/README.md) |

Усі режими, назви executable, команди та очікувані результати є в
[каталозі прикладів](EXAMPLES.md). Файли `platform_fake.h`, `LoopPort.h`
та блоки `DOC_HOST` — стенд для ПК, не HAL/RTOS для копіювання у прошивку.

## Словник типів: що з чим не плутати

| Назва | Простими словами | Що робиш ти |
|---|---|---|
| `Endpoint` | один екземпляр протокольного каналу: RX, TX, черги готових пакетів, лічильники | створюєш один на канал/потік |
| `Format` | правила дроту та максимальні корисні дані | зазвичай залишаєш `<>`; змінюєш лише за потреби |
| `Memory` / storage policy | звідки брати й куди повертати пам'ять | `wire::Heap`, `wire::Pool<8, 2>` або власний тип |
| `Geometry` | уже пораховані максимальні фізичні розміри й alignment | потрібна лише автору власного storage |
| CRC policy | як порахувати, записати й прочитати трейлер | дефолт, Table, NoCrc або свій калькулятор |
| RTU framer | як за функцією/полями визначити, скільки байтів чекати | обираєш RX Request або RX Response; TCP цього не потребує |
| `Message` | твій змінюваний пакет для відправлення, один власник | заповнюєш, перевіряєш append, передаєш у send |
| `Packet` | готові незмінні вхідні дані зі спільним володінням | читаєш; можеш зробити копію handle в тому самому контексті |
| `Sender` | делегат «прийми цілий кадр на TX» | даєш транспорту або за тебе його прив'язує адаптер |
| `BusyQuery` | делегат «чи транспорт ще зайнятий/утримує TX» | правдиво відповідає транспорт |
| `UartAdapter` | уже з'єднані RX, gap, send, busy, poll і потрібний RTU stale-deadline | викликаєш `bind()`, потім `proceed()` |
| `FreeRtosWake` | будильник задачі, не парсер і не черга пакетів | attach до UART у своїй задачі, потім `wait(...)` |

Делегат — типізований callback. Функція-член через `tiny::bind` позичає
об'єкт; той має жити довше за binding. Лямбди також підтримуються, включно
із захопленнями, але захоплений вказівник/посилання не продовжує життя об'єкта.
У гарячих callback-ах endpoint-а не кидай винятків і не викликай його повторно
зсередини тієї ж операції.

## Політики без магії

Типова конфігурація, фрагмент оголошень:

```cpp
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"

using Memory = wire::Pool<8, 2>;
using Cobs = cobs::Endpoint<Memory>;          // CRC16, 253 корисні байти
using Rtu = modbus::rtu::Endpoint<Memory>;    // CRC16, 252 байти function data
using Tcp = modbus::tcp::Endpoint<Memory>;    // NoCrc, 252 байти function data
```

### Heap чи Pool

`Endpoint<>` використовує Heap. Нічого рахувати наперед не треба, але
алокація може відмовити. Поточний Heap використовує `malloc/free`;
налаштування глобального `new` саме по собі його не змінює.

`wire::Pool<8, 2>` означає **8 RX-власників і 2 TX-власники**, не 8/2 байти.
Скільки байтів має бути у слоті, визначає endpoint із Format. RX-слот може
утримуватися незавершеним, готовим у черзі або збереженим користувачем Packet.
TX-слот зайнятий Message або активним надсиланням. Копія Packet не бере
другий слот, але відкладає повернення першого.

Для початку залиш Pool; переходь на Heap, якщо потрібна динамічна пам'ять.
Власний storage не потрібен для звичайного користування. Якщо він потрібен,
реалізуєш чотири методи із [Storage](STORAGE.md), не структуру Packet.

### Як змінити розмір даних

```cpp
using LargeCobs = cobs::Endpoint<wire::Pool<8, 2>,
    cobs::Format<crc::Crc16Bitwise, 1024>>;
using LargeRtu = modbus::rtu::Endpoint<wire::Pool<8, 2>,
    modbus::rtu::Format<crc::Crc16Bitwise, 1024>>;
using LargeTcp = modbus::tcp::Endpoint<wire::Pool<8, 2>,
    modbus::tcp::Format<crc::NoCrc, 1024>>;
```

У кожному випадку це 1024 байти `data()`. Нічого віднімати за CRC, MBAP чи
COBS не треба. Однак такі великі RTU/TCP кадри — приватне розширення, не
стандартний Modbus. Менший ліміт — лише твоя локальна місткість.
COBS також дозволяє окремі RX/TX ліміти: `Format<CRC, MaxRx, MaxTx>`.
Обидва кінці мусять погодити CRC та ширину length; автовизначення немає.

### Bitwise, Table, NoCrc, периферія

- `crc::Crc16Bitwise` — дефолт COBS/RTU, без lookup-таблиці.
- `crc::Crc16Table` — той самий CRC й дріт, інший спосіб обчислення.
- `crc::NoCrc` — немає трейлера і перевірки цілісності; стандартний дефолт TCP.
- `cobs::Format<crc::NoCrc, 255>` — старий COBS v1 без CRC; увімкни явно, якщо потрібен саме він.
- CRC8/32/64 та власний stateful calculator — [CRC guide](../src/crc/README.md).
- Реальна STM32-політика — [Crc16.h](../src/adapters/stm32/Crc16.h) та [вимірювання/використання](HEAP_AND_HARDWARE_CRC.md).

У невикористаних Table-спеціалізацій немає таблиці в образі; це перевіряють
окремі object/codegen тести. Використаний Table тримає спільну read-only
таблицю, не таблицю в кожному Endpoint. Власну суму теж можна передати:
бібліотека перевіряє контракт типу, а не «правильність Modbus» твого алгоритму.
Не-Modbus checksum у RTU/TCP — свідомий приватний формат обох кінців.

## Однаковий цикл Message і Packet

Повний виконуваний файл: [protocols.cpp](examples/protocols.cpp).

1. Створи endpoint і прив'яжи транспорт або адаптер.
2. `make_message(...)` створює Message. Останнє число — capacity hint, не готові дані.
3. Перевір Message та **кожен** `append_*`. Помилковий append не додає поле і не забороняє send автоматично.
4. `send(message)` повертає результат; збережи Message, якщо треба повторити.
5. Вхідні байти потрапляють у `consume()` або цілий RTU ADU у `receive_adu()`.
6. `pop_packet()` віддає готовий Packet. `data()` не містить протокольних заголовків/CRC.
7. `proceed()` адаптера або `poll(now_ms)` для ручного транспорту повертає завершений TX-блок.

### Метадані задаються один раз

| Протокол | Створення | Додаткові поля Packet |
|---|---|---|
| COBS | `make_message(hint)` | `data()`, `size()` |
| RTU | `make_message(address, function, hint)` | `address()`, `function()`, `pdu()`, `adu()` |
| TCP | `make_message(transaction_id, unit_id, function, hint)` | `transaction_id()`, `unit_id()`, `function()`, `pdu()`, `adu()` |

### Читання та запис полів

`CHECK(...)` у цих виконуваних прикладах зупиняє тест при помилці; це не
метод бібліотеки. У застосунку замість нього використовуй `if (!...)` та
власну обробку відмови, як у повних прикладах задачі/Qt нижче за посиланнями.

<!-- example: examples/protocols.cpp#write-fields -->
```cpp
auto message = make(link);
CHECK(message && message.size() == 0u); // hint is capacity, not size
CHECK(message.append_be(uint16_t{0x1234}));
CHECK(message.append_le(uint32_t{0x10203040}));
const std::array<uint8_t, 3> bytes{0u, 1u, 2u};
CHECK(message.append_bytes(bytes));
CHECK(link.send(message) == wire::SendResult::Sent);
CHECK(!message); // ownership transferred, not an acknowledgement
```
<!-- /example -->

Цей уривок використовує `make(link)` із повної програми для вибору протоколу.
BE/LE задають порядок байтів поля явно. `append_native(value)` використовує
представлення поточної платформи; застосовуй його лише коли обидва боки
справді погодили таке представлення. Для масивів writer має span-overload.
Reader чисел читає одне значення за виклик; масив читається циклом.

<!-- example: examples/protocols.cpp#read-fields -->
```cpp
std::size_t offset = 0;
uint16_t first = 0;
uint32_t second = 0;
std::span<const uint8_t> tail;
CHECK(wire::read_be(packet.data(), offset, first));
CHECK(wire::read_le(packet.data(), offset, second));
CHECK(wire::read_bytes(packet.data(), offset, 3u, tail));
CHECK(first == 0x1234u && second == 0x10203040u && std::ranges::equal(tail, bytes));
CHECK(!wire::read_be(packet.data(), offset, first));
CHECK(offset == 9u && first == 0x1234u); // failed read changes neither

auto retained = packet; // same immutable allocation, not another copy of data
packet.reset();
CHECK(retained && retained.size() == 9u);
retained.reset(); // final reference returns the RX block
```
<!-- /example -->

`read_bytes` повертає view на дані Packet, а не власну копію. View живе,
поки живе власник Packet. Для GUI/черги іншої задачі скопіюй самі дані.
`Packet::reset()` відпускає handle. Для скидання Message використовуй
`message = {}` — це різні типи, Message не має `reset()`.

### Що означає send result

| Результат | Message після виклику | Твоя дія |
|---|---|---|
| `Sent` | порожній, блок перейшов endpoint-у | дочекайся завершення транспорту; це не ACK від peer |
| `Busy` | збережений | лиши його в полі/черзі й повтори пізніше |
| `Failed` | збережений, але вже finalized/read-only | виріши відновлення транспорту; повтор можливий тими самими байтами |
| `Unbound` | збережений | налаштуй binding або явно відкинь |
| `Invalid` | немає придатного повідомлення/owner або не збігається відома RTU layout | виправ побудову; не крути нескінченний retry |

Не створюй відповідь як локальну змінну, якщо при Busy плануєш відправити її
в наступній ітерації: вона знищиться на виході. Приклад збереженої відповіді
та двома запитами підряд — [rtu_adapter.cpp](examples/rtu_adapter.cpp).
Окремий тест усіх трьох протоколів — [backpressure.cpp](examples/backpressure.cpp).

## Адаптер, wake чи ручне підключення

```text
UART ISR -> фіксує подію / будить задачу
                    |
                    v
задача: wait(adapter) -> adapter.proceed() -> endpoint -> pop_packet()
```

Wake не викликає парсер. `proceed()` виконує роботу після пробудження.
COBS не потребує таймера незавершеного кадру. RTU-адаптер керує своїм
stale-deadline. У звичайній задачі не потрібні ні `std::min`, ні
`deadline_in_ms()`, ні ручне передавання `now`:

```cpp
(void)uart::FreeRtosWake::wait(adapter);
adapter.proceed();
```

Без RTOS — просто `adapter.proceed()` у головному циклі. Без протокольного
адаптера ти сам прив'язуєш RX/gap/send/busy та викликаєш `serial.proceed(now)`
і `endpoint.poll(now)`. `FreeRtosWake` можна залишити навіть тоді: він
прив'язаний до UART, а не до COBS/RTU. Для голого UART використовуй `wait(50u)`.
Повна таблиця комбінацій — [Integration](INTEGRATION.md#supported-combinations).

## Де протоколи справді відрізняються

### RTU: цілий ADU проти потоку

`modbus::rtu::Endpoint<>` без framer приймає **один цілий кандидат** через
`receive_adu()`. Будь-які function codes допустимі. Він не знає, чи шматок
із UART уже є цілим кадром. `Uart<256, 4>` не гарантує цього: IDLE може
розрізати ADU, а кілька ADU можуть прийти разом.

Для потоку стандартних запитів обирай `Standard<Direction::Request>` на
сервері; для відповідей — `Standard<Direction::Response>` на клієнті.
Напрямок — що **приймаєш**. Запит і відповідь однієї функції можуть мати
різну довжину, тому автоматично змішувати ролі не можна. Приклад обох ролей,
exception і власної функції — [rtu_framing.cpp](examples/rtu_framing.cpp).

Невідома функція на RX не має правила довжини й не видається як Packet.
На TX невідома layout не забороняє надсилання; перевірка відомої layout не
є повною валідацією всіх Modbus-значень. Фреймер не рахує CRC й не реалізує
строгі фізичні t1.5/t3.5. Деталі та перелік підтриманих функцій — [RTU guide](../src/modbus/README.md).

### TCP: MBAP, а не таблиця функцій

`consume()` складає кадр за MBAP Length, навіть для приватної функції.
Transaction ID зіставляє твоя логіка запитів, core цього не робить.
Невалідний MBAP або gap переводить RX у failed. Закрий зіпсований потік;
`reset_rx()` — лише для відомого нового потоку, не «спробуй наступний chunk».
OOM відрізняється: відома довжина дозволяє пропустити рівно цей кадр.

### COBS: delimiter і конфігурація обох боків

На дроті кодується `[length(payload + CRC)][payload][CRC]`, після чого
додається `00`. `length` — число, а не друга копія CRC. Packet повертає
лише payload. Після фізичного gap парсер відкидає до наступного delimiter.
Peer із NoCrc може прийняти CRC-байти за корисні дані: автоматичного
розпізнавання версії/CRC немає. [Точний wire contract](PROTOCOL.md).

## Якщо нічого не працює

| Симптом | Спершу перевір |
|---|---|
| `make_message()` порожній | hint ≤ max data; чи не зайняті TX-слоти; чи є пам'ять Heap |
| `send()` весь час Busy | чи завершується TX; чи викликається proceed/poll; чи transport busy правдивий |
| RX ISR є, пакетів нема | чи задача робить proceed; чи wake на її handle; чи збігається Format і роль RTU |
| CRC errors / дивні зайві байти | однакові CRC/length settings обох кінців, а не лише baud |
| RTU працює лише на малих швидкостях | чи не сприймаєш UART chunks як повні ADU |
| TCP після одного пошкодження мовчить | `rx_failed()`; потрібен новий потік, не довільний reset |
| Pool «закінчився» | готові й збережені Packets також тримають RX-слоти |
| Дані псуються після callback | збережено span без Packet або змінено активний DMA TX-буфер |
| Qt нічого не викликає | event loop, той самий thread, handler після bind, lifetime об'єктів |
| FreeRTOS прокидається надто часто | tick/deadline/fallback; не запускай wait з іншої задачі або ISR |

Загальне правило завершення: припини нові відправлення, дочекайся завершення
або підтвердженого abort транспорту, поверни TX через poll/proceed, відв'яжи
callback-и, відпусти всі Message/Packet, і лише потім знищуй власників.
Для UART не вважай `HAL_UART_STATE_READY` доказом зупинки DMA.

## Як переконатися, що приклад не застарів

```sh
python -B doc/check_docs.py
sh doc/examples/build.sh
sh doc/examples/qt/build.sh
```

Перший перевіряє навігацію та синхронізовані уривки; другий — portable code
і host fake HAL/FreeRTOS; третій — справжній Qt event loop і localhost TCP.
Це не перепрошивання плати. Для всієї regression-матриці та збережених
живих тестів відкрий [Testing](TESTING.md). Останній повний H7S checkpoint:
[81 образ, 2026-09-12](HARDWARE_EXTENSIONS_2026-09-12.md).
