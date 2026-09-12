<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Documentation preservation: what moved and what was restored

<!-- toc -->

Contents

- [Межі перевірки та вихідні версії](#межі-перевірки-та-вихідні-версії)
- [Куди перенесені розділи старого README](#куди-перенесені-розділи-старого-readme)
- [Куди перенесені integration-сценарії](#куди-перенесені-integration-сценарії)
- [Що справді було загублено і повернуто](#що-справді-було-загублено-і-повернуто)
- [Які старі твердження не можна повертати](#які-старі-твердження-не-можна-повертати)
- [Що видалено з дерева і як це відновити](#що-видалено-з-дерева-і-як-це-відновити)
- [Подальше збереження документації](#подальше-збереження-документації)

<!-- /toc -->

[Documentation](README.md) · [Почни звідси](START_HERE_UK.md) · [Examples](EXAMPLES.md) · [Testing](TESTING.md)

Цей документ відповідає на запитання: «Чи не загубилася потрібна інформація
після скорочення README та перегрупування документації?» Це карта перенесення
й виправлень, а не твердження, що кожне старе речення досі правильне.

## Межі перевірки та вихідні версії

Перевірено такі зрізи Git для README, Integration, Build та Storage:

- `3ef8abe` — перший окремий integration guide з виконуваними прикладами;
- `3ef8abe → 1b20c83` — перенесення під `src/`, Qt, вирівнювання API й автоматичний час адаптерів;
- `1b20c83 → a5991df` — TCP, ліміти корисних даних, Heap/OOM, reader/CRC/wake contracts;
- `a5991df → 96323a7` — реорганізація документації, нові Qt/FreeRTOS приклади й погоджене видалення `doc/old`;
- поточне доповнення — відновлення знайдених прогалин, перелічених нижче.

В останньому зрізі також переглянуто diff решти змінених чинних документів
та старих example-файлів. Нові місця звірено зі змістом видалених розділів,
а не лише з назвами файлів. Це не повторний аудит усієї історії протоколу
чи повтор усіх апаратних вимірювань за весь час існування репозиторію.

Старі версії доступні без перезапису робочих файлів:

```sh
git show a5991df:README.md
git show a5991df:doc/INTEGRATION.md
git diff a5991df 96323a7 -- README.md doc/INTEGRATION.md doc/examples
git diff 3ef8abe a5991df -- README.md doc/INTEGRATION.md doc/BUILD.md doc/STORAGE.md
```

Під час зміни include-шляхів `src/` іменування змінилося, але це не видалення
самої бібліотеки. `frames_delivered → frames_received` та `MaxAdu → MaxData`
є зафіксованими змінами API: [API parity](API_PARITY.md#source-migration-in-this-slice)
і [payload migration](PAYLOAD_LIMITS.md#migrating-earlier-explicit-modbus-limits).

## Куди перенесені розділи старого README

Назви зліва походять із `a5991df:README.md`. Підрозділи згруповані за темою;
праворуч — чинна документація, яку можна читати без відкривання реалізації.

| Попередній зміст | Чинне місце / рішення |
|---|---|
| Назва CRC/COBS/RTU/TCP/DMA/C++20, бейджі, автор, перелік можливостей | [кореневий README](../README.md): відновлено, а не замінено короткою назвою |
| What is in this repository? | [карта шарів та include-файлів](USER_GUIDE.md#what-is-in-this-repository) |
| Highlights | [можливості та обмеження](USER_GUIDE.md#highlights) |
| Requirements; clone/submodule commands | [вимоги й отримання залежностей](USER_GUIDE.md#requirements) |
| COBS quick start; Formats and storage | [COBS та вибір Format/Memory](USER_GUIDE.md#cobs-quick-start) |
| Bind a byte transport | [Sender/BusyQuery](USER_GUIDE.md#bind-a-byte-transport) |
| Build and send a message; append/read scalar rules | [побудова, SendResult, native/BE/LE](USER_GUIDE.md#build-and-send-a-message) |
| Receive bytes and packets; gap recovery | [consume, Packet, readers, notify_gap](USER_GUIDE.md#receive-bytes-and-packets) |
| COBS ownership and execution rules | [lifetime, non-atomic Packet, re-entry](USER_GUIDE.md#cobs-ownership-and-execution-rules) |
| COBS diagnostics and pool pressure | [лічильники й вичерпання пулу](USER_GUIDE.md#cobs-diagnostics-and-pool-pressure) |
| COBS API at a glance | [таблиця публічного API](USER_GUIDE.md#cobs-api-at-a-glance) |
| Modbus RTU quick start | [короткий початок](USER_GUIDE.md#modbus-rtu-quick-start), [повний RTU guide](../src/modbus/README.md) |
| STM32 UART quick start; Callback integration | [режими callback та повне forwarding-підключення](USER_GUIDE.md#callback-integration) |
| Initialize and service UART | [init/proceed, обробники, часовий контекст](USER_GUIDE.md#initialize-and-service-uart) |
| Event-driven servicing under an RTOS | [короткий цикл](USER_GUIDE.md#event-driven-servicing-under-an-rtos), [повний FreeRTOS guide](FREERTOS.md) |
| UART receive/transmit contracts | [RX](USER_GUIDE.md#uart-receive-contract), [TX borrow](USER_GUIDE.md#uart-transmit-contract) |
| Runtime baud change | [setBaudRate та навмисний gap](USER_GUIDE.md#runtime-baud-change) |
| UART diagnostics; UART API at a glance | [лічильники](USER_GUIDE.md#uart-diagnostics), [усі основні виклики](USER_GUIDE.md#uart-api-at-a-glance) |
| Complete UART + COBS composition | [SerialStack, pending Message, RX і TX](USER_GUIDE.md#complete-uart--cobs-composition) |
| Wire protocol | [коротке пояснення](USER_GUIDE.md#wire-protocol), [нормативний формат](PROTOCOL.md) |
| Build integration; qmake/CMake | [чинні include/source recipes](BUILD.md#embed-the-libraries-in-your-application), [qmake та override delegate](BUILD.md#reusable-cobs-qmake-fragment) |
| Verification commands and consumer sources | [каталог перевірок](TESTING.md), включно з COBS host benchmark і qmake consumer sources |
| Raw hardware links; performance paragraphs | [прямий індекс старих результатів](TESTING.md#historical-raw-records-direct-links-retained), [COBS performance](COBS_PERFORMANCE.md), [COBS/RTU comparison](PROTOCOL_COMPARISON.md) |
| Latest audits, migration plans, documentation map | [індекс документації, звітів і планів](README.md) |
| Common questions: 255/256, Pool counts, no TX queue, HT, cross-task Packet, overrun | [усі шість FAQ](USER_GUIDE.md#common-questions) |
| License and third-party notices | [ліцензія](../LICENSE), [сторонні компоненти](../THIRD_PARTY_NOTICES.md) |

## Куди перенесені integration-сценарії

| Старий розділ / приклад | Чинне місце / рішення |
|---|---|
| What every pattern shares | [спільна побудова та ownership](INTEGRATION.md#common-setup-and-ownership), [API parity](API_PARITY.md) |
| RTU on STM32 through UartAdapter | [підключення через адаптер](INTEGRATION.md#stm32-with-an-adapter), [перевірений server source](examples/rtu_adapter.cpp) |
| prepare / UART proceed / finish / Endpoint poll | [повний порядок та контракт](INTEGRATION.md#rtu-adapter-split-servicing-and-lifecycle), `rtu_adapter_split` у cookbook |
| COBS through UartAdapter | [адаптерний сценарій](INTEGRATION.md#stm32-with-an-adapter), [cobs_adapter.cpp](examples/cobs_adapter.cpp) |
| COBS direct wiring | [ручний RX/gap/send/busy та loop](FREERTOS.md#cobs-with-wake-but-without-uartadapter), [cobs_direct.cpp](examples/cobs_direct.cpp) з wake і без |
| FreeRTOS task creation, wake and service | [повна task entry](FREERTOS.md#complete-communication-task), [кожна wake-операція](FREERTOS.md#wake-api-every-public-operation) |
| RTU on STM32 without adapter | [повний ручний UART/RTU client](INTEGRATION.md#complete-manual-rtu--uart-client), [task/wake варіант](FREERTOS.md#rtu-with-wake-but-without-uartadapter) |
| Bare RTU whole-candidate API | [receive_adu contract](INTEGRATION.md#bare-rtu-requires-a-whole-candidate), [rtu_direct.cpp](examples/rtu_direct.cpp); збережено окремо від UART-прикладу |
| Manual stale timer skeleton | помилковий код не повернуто; ручний client має явно іншу, обмежену політику загального бюджету запиту |
| Qt on desktop | [повний Qt guide](QT.md): COBS, queued RTU client, RTU server, errors, teardown та TCP sockets |
| Any other byte transport | [контракт транспорту](INTEGRATION.md#your-own-byte-transport), [RTU source](examples/any_transport.cpp), [усі три протоколи](examples/protocols.cpp) |
| Choosing parameters | [START_HERE](START_HERE_UK.md), [Storage](STORAGE.md), [CRC](../src/crc/README.md), [useful-data limits](PAYLOAD_LIMITS.md) |
| Execution and lifetime rules | [ownership](INTEGRATION.md#common-setup-and-ownership), [shutdown](INTEGRATION.md#error-handling-and-shutdown), [IRQ/task context](FREERTOS.md#irq-priorities-ownership-and-task-shutdown) |

Старий `any_transport.cpp` зайво прив'язував два протоколи до одного
транспорту, не реалізуючи multiplexing. Окремі повні приклади збережено;
це не обіцянка одночасно розрізняти COBS та RTU на одному UART без додаткового
wire-протоколу. Старий FreeRTOS fake `xTaskCreate` замінено реальною task entry
та окремими явно позначеними host fixtures, а не видалено сценарій запуску task.

## Що справді було загублено і повернуто

1. Шапка README: повна назва, бейджі, автор та вступний перелік можливостей.
2. UART-приклад ручного RTU: новий повний source та inline-код, з wake і без,
   з Busy, gap, timeout, exception, wrap і безпечним завершенням TX borrow.
3. Розширений RTU adapter recipe `prepare/finish`, його порядок, clock/lifecycle
   контракт і перевірки для обох форм обслуговування.
4. Явний запуск COBS host benchmark та посилання на його методику.
5. Прямі посилання на старі raw results та application-shaped qmake sources.

Найбільш непомітна втрата була в навігації: два COBS JSONL за 2026-09-01
і UART CSV 128x8/10M залишалися на диску, але втратили єдині Markdown-посилання.
Тепер [індекс raw records](TESTING.md#historical-raw-records-direct-links-retained)
містить не лише ці три файли, а весь попередній перелік.

Окремо порівняно всі 65 унікальних локальних destinations старого README:
64 досі мають прямі Markdown-посилання в чинних посібниках. Єдиний виняток —
погоджено видалена папка `doc/old`, для якої є Git recovery. Ця перевірка
підтверджує збереження навігації; зміст розділів перевірено окремо за картою вище.

## Які старі твердження не можна повертати

| Старий текст / підхід | Чому замінено |
|---|---|
| UART IDLE та ChunkSize >= 256 гарантують цілий RTU ADU | bridge може розрізати кадр або склеїти доставки; потрібен whole-candidate контракт або stream framer |
| Усі clocks повністю зовні, жодна бібліотека не читає час | готові STM32-адаптери вже мають автоматичний HAL-time API; explicit-time API теж задокументовано |
| Delegates допускають лише посилання та capture-less lambdas | tiny::delegate також зберігає callables; запозичені targets мають власний lifetime контракт |
| Attach wake після створення runnable task | task уже може виконуватися; тепер attach відбувається з owning task до UART init |
| Ненульовий rx_progress означає новий прогрес на кожному deadline | frozen counter міг нескінченно подовжувати старий таймер; empty input теж не повинен його пересувати |
| Short serial write не залишає частини кадру на дроті | очищення черги не відкликає вже передані байти; retry потребує рішення застосунку |
| 50-ms incomplete-frame timer стосується також COBS | COBS не має такого таймера: delimiter/gap/discard — окремі механізми |
| NoCrc у RTU автоматично дає 254 data bytes за новими defaults | MaxData тепер фіксує корисні байти; default 252 зберігається, змінюється ADU size |
| Heap використовує nothrow new та користувацькі global new hooks | після підтвердженого nano-runtime OOM дефолтний Heap використовує malloc/free; [чинний контракт](STORAGE.md#built-in-memory-specifications) описує цю межу |

Старі значення тестових лічильників і CPU-замірів залишаються в датованих
звітах. Їх не переписано новими числами й не подано як свіжі результати.

## Що видалено з дерева і як це відновити

Єдине погоджене видалення цілих історичних файлів у документаційному коміті —
23 файли `doc/old`, не частина чинної збірки. Повний перелік, причина,
відмінності legacy UART/RS485/IT/circular DMA та відновлення з Git — у
[LEGACY_REVIEW.md](LEGACY_REVIEW.md). Ця перевірка не повертає старі реалізації
під виглядом чинного API.

У `96323a7` єдина зміна активного C++ header — історичний коментар у
`src/uart/Uart.h`. У поточному доповненні production-код не змінюється.
Hardware JSON/JSONL/CSV, ELF та firmware images не переписуються і плата не
перепрошивається. Приклади та їхні host/compile checks — окрема категорія.

## Подальше збереження документації

При наступній реорганізації кожен видалений розділ повинен отримати одне з
трьох пояснень: посилання на нове місце, перевірену заміну прикладу або
конкретну причину, чому старе твердження неправильне. «Посилання проходять»
не доводить повноти: видалене посилання саме по собі не є broken link.

Ця карта входить до `check_docs.py`: її локальні targets та anchors теж
перевіряються. Додатковий guard фіксує критичні маршрути до посібника,
ручного RTU, benchmark та raw results: видалення самого посилання теж тепер
помилка, навіть якщо target-файл існує. Negative tests окремо перевіряють
цей сценарій та збереження назви/бейджів README.
Cookbook-код у посібниках синхронізований із компільованими
source regions. [Testing](TESTING.md#documentation-preservation-follow-up-2026-09-13)
відокремлює актуальні перевірки цього доповнення від попередніх звітів.
