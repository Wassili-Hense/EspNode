# EspNode

Узел X13 (MQTT-SN) для ESP32 на ESP-IDF — перенос прошивки X13.Home (`dev_320`).

Переносятся транспорты MQTT-SN, Object Dictionary и PLC. Ввод-вывод будет только через функции PLC
(пока не реализован).

- MQTT-SN поверх UART, совместимый со шлюзом X13/Enviriot (кадр `[len][SLIP-escaped][0xC0]`,
  DHCP-адресация 0x43/0x44, predefined-объекты 0xFFxx).
- PLC: байткод компилятора Enviriot с float-командами, загрузка через объект `pa0`, переменные —
  мерке́ры `M…` ([docs/plc-api.md](docs/plc-api.md)).

## Структура

| Компонент | Назначение |
|---|---|
| `components/mqttsn` | Протокол: сборка сообщений, state machine клиента (DHCP → SEARCHGW → CONNECT …) |
| `components/serial_phy` | Транспорт UART/RS485, кодек кадров X13 |
| `components/objdict` | Object Dictionary: predefined-объекты, регистрация, упаковка значений, NVS |
| `components/plc` | VM (целые и float), управление программой, мерке́ры, API функции, хранение программы |
| `main` | Задача узла (тик 10 мс), настройки в `menuconfig → EspNode` |
| `tools/gw_sim.py` | Симулятор шлюза X13 для проверки без X13 |

## Сборка

ESP-IDF ≥ 5.3 (проверено на v6.1), target `esp32`, flash ≥ 2 МБ.

```
idf.py -B build/default build
idf.py -B build/default -p COM3 flash monitor
```

`-B build/default` обязателен: в проекте лежит `CMakePresets.json` (по нему собирает расширение ESP-IDF
для VS Code), а `idf.py` v6.1 не раскрывает `${sourceDir}` в его пути и без ключа создаёт каталог
с буквальным именем `${sourceDir}`. Из VS Code собирается без ключа.

Таблица разделов своя (`partitions.csv`, раздел `plc` для программы PLC): при переходе с прошивки
без PLC нужна полная прошивка (`idf.py flash`), настройки в NVS сохраняются.

## Подключение

По умолчанию MQTT-SN идёт через UART1: TX = GPIO17, RX = GPIO16, 38400 8N1; консоль и логи — UART0.
Для RS485 задайте `ESPNODE_UART_DE_PIN` (DE и /RE трансивера соединены).

## Проверка с симулятором

```
pip install pyserial
python tools/gw_sim.py --selftest
python tools/gw_sim.py COM5          # USB-UART адаптер на GPIO17/16
```

На чистом узле: DHCPREQ → SEARCHGW → CONNECT `E1S10_10` → REGISTER `pa0` → PUBLISH 0xFFC0
`E1S10.400`, 0xFFC1 → SUBSCRIBE `#` → состояние PLC. Команды: `name kitchen` — имя узла,
`plc start|stop|crc`, `reg 50 Md0` — мерке́р.

## Тесты PLC

Программы для ручной проверки на устройстве — [tests/plc](tests/plc/README.md).

## План и история

[TODO.md](TODO.md) — что предстоит, [changelog.md](changelog.md) — что и почему менялось.

## Лицензия

MIT (`LICENSE`). Код, перенесённый из X13.Home, — BSD 3-Clause (`LICENSE.X13`).
