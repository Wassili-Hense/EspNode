# EspNode

Узел X13 (MQTT-SN) для ESP32 на ESP-IDF — перенос прошивки X13.Home (`dev_320`).

Шаг 1: MQTT-SN клиент поверх UART, совместимый со шлюзом X13 (кадр `[len][SLIP-escaped][0xC0]`,
DHCP-адресация 0x43/0x44, predefined-объекты 0xFFxx).

## Структура

| Компонент | Назначение |
|---|---|
| `components/mqttsn` | Протокол: сборка сообщений, state machine клиента (DHCP → SEARCHGW → CONNECT …) |
| `components/serial_phy` | Транспорт UART/RS485, кодек кадров X13 |
| `components/objdict` | Object Dictionary: predefined-объекты, регистрация, упаковка значений, NVS |
| `main` | Задача узла (тик 10 мс), настройки в `menuconfig → EspNode` |
| `tools/gw_sim.py` | Симулятор шлюза X13 для проверки без X13 |

## Сборка

ESP-IDF ≥ 5.3 (проверено на v6.1), target `esp32`.

```
idf.py -B build/default build
idf.py -B build/default -p COM3 flash monitor
```

`-B build/default` нужен из-за `CMakePresets.json` (создаётся расширением ESP-IDF для VS Code):
`idf.py` v6.1 не раскрывает `${sourceDir}` в пресете. Из VS Code собирается без этого ключа.

## Подключение

По умолчанию MQTT-SN идёт через UART1: TX = GPIO17, RX = GPIO16, 38400 8N1; консоль и логи — UART0.
Для RS485 задайте `ESPNODE_UART_DE_PIN` (DE и /RE трансивера соединены).

## Проверка с симулятором

```
pip install pyserial
python tools/gw_sim.py --selftest
python tools/gw_sim.py COM5          # USB-UART адаптер на GPIO17/16
```

На чистом узле: DHCPREQ → SEARCHGW → CONNECT `E1S10_10` → PUBLISH 0xFFC0 `E1S10.400`, 0xFFC1 →
SUBSCRIBE `#` → PINGREQ. Команда `name kitchen` задаёт имя узла (ClientId после переподключения).

## Лицензия

MIT (`LICENSE`). Код, перенесённый из X13.Home, — BSD 3-Clause (`LICENSE.X13`).
