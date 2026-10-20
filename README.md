# EspNode

Узел X13 (MQTT-SN) для ESP32 на ESP-IDF — перенос прошивки X13.Home (`dev_320`).

Переносятся транспорты MQTT-SN, Object Dictionary и PLC. Ввод-вывод настраивает и выполняет программа
PLC: пины, ШИМ, АЦП, ЦАП, счётчики, шины TWI и UART.

- MQTT-SN поверх UART, совместимый со шлюзом X13/Enviriot (кадр `[len][SLIP-escaped][0xC0]`,
  DHCP-адресация 0x43/0x44, predefined-объекты 0xFFxx).
- PLC: байткод компилятора Enviriot с float-командами, загрузка через объект `pa1` (протокол `pa0` X13), переменные —
  мерке́ры `M…`, ввод-вывод — `ConfigPin` и опкоды `IN`/`OUT` по номеру GPIO
  ([docs/plc-api.md](docs/plc-api.md)). Что нужно в Enviriot — [docs/enviriot-todo.md](docs/enviriot-todo.md).

## Структура

| Компонент | Назначение |
|---|---|
| `components/mqttsn` | Протокол: сборка сообщений, state machine клиента (DHCP → SEARCHGW → CONNECT …) |
| `components/serial_phy` | Транспорт UART/RS485, кодек кадров X13 |
| `components/objdict` | Object Dictionary: predefined-объекты, регистрация, упаковка значений, NVS |
| `components/plc` | VM (целые и float), управление программой, мерке́ры, API функции, ввод-вывод, хранение программы |
| `components/node_cfg` | Раздел `cfg`: разбор и проверка настроек узла со страницы установки |
| `main` | Задача узла (тик 10 мс), применение `cfg`; значения без `cfg` — в `menuconfig → EspNode` |
| `tools/gw_sim.py` | Симулятор шлюза X13 для проверки без X13 |
| `tools/mkcfg.py` | Образ раздела `cfg` из командной строки |
| `site/`, `tools/mkmanifest.py` | Страница установки; сайт: страница, прошивка, `manifest.json` |
| `.github/workflows` | Сборка и тесты в CI, публикация страницы на GitHub Pages |
| `test/host` | Хостовые тесты: VM, ввод-вывод, `cfg`, сквозные MQTT-SN и PLC |

## Сборка

ESP-IDF ≥ 5.3 (проверено на v6.1), target `esp32`, flash 4 МБ.

```
idf.py -B build/default build
idf.py -B build/default -p COM3 flash monitor
```

`-B build/default` обязателен: в проекте лежит `CMakePresets.json` (по нему собирает расширение ESP-IDF
для VS Code), а `idf.py` v6.1 не раскрывает `${sourceDir}` в его пути и без ключа создаёт каталог
с буквальным именем `${sourceDir}`. Из VS Code собирается без ключа.

Таблица разделов своя (`partitions.csv`: `plc` — программа PLC, `cfg` — настройки узла, адреса —
[docs/cfg-format.md](docs/cfg-format.md)): после её изменения нужна полная прошивка (`idf.py flash`).

Консоли нет: без `cfg` MQTT-SN занимает UART0, а текст в линии мешает шлюзу. Отладочная сборка —
с консолью и журналом загрузчика на UART0 и MQTT-SN на UART1 (GPIO17/16, через USB-UART адаптер):

```
idf.py -B build/default -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.debug" build
```

ESP-IDF v6.1 помечает в `sdkconfig` значения по умолчанию (`# default:`) и заменяет их значениями из
`sdkconfig.defaults` и `sdkconfig.debug`; выбранное в `menuconfig` остаётся. Консоль и MQTT-SN на одном
UART — ошибка сборки.

Версия: тег `vX.Y.Z` даёт версию в типе устройства `E1S10.XYZ`; без такого тега — `ESPNODE_SW_VERSION`
из `menuconfig`.

## Страница установки

[https://wassili-hense.github.io/EspNode/](https://wassili-hense.github.io/EspNode/) — Chrome, Edge или Opera на
компьютере (Web Serial). Плату подключить по USB, закрыть программы, которые держат её порт, и нажать
«Подключить». Страница показывает чип, MAC, flash и что уже стоит на плате: версию EspNode и настройки
`cfg`, которыми заполняет поля. Дальше:

- **«Установить»** — прошивка и `cfg`. Галочка «Стереть настройки узла и программу PLC» стоит для новой
  платы (на ней нет ни EspNode, ни `cfg`): остатки прежней прошивки иначе запустятся как программа PLC.
- **«Только настройки»** — один `cfg`, программа и объекты узла не трогаются.

Записанное проверяется по MD5, и порт освобождается для Enviriot. После записи нажмите на плате Reset
(EN или RST): перезапуск со страницы срабатывает не на всех платах. Пока доступен
шаблон Serial (UART0 — тот же USB, 38400); WiFi и WiFi + RS-485 появятся с прошивкой, которая их умеет.

Страница — ES-модули в `site/` без сборки, `esptool-js` берётся с jsDelivr с проверкой хеша. Тот же сайт
локально, из своей сборки (Web Serial работает и на `localhost`). Для проверки сборка нужна без
`sdkconfig.debug`: с консолью на UART0 прошивка отвергнет `cfg` шаблона Serial по умолчанию.

```
python tools/mkmanifest.py build/default _site
python -m http.server -d _site
```

## GitHub Actions

- `build` — прошивка, хостовые тесты и тесты модулей страницы на каждый push (кроме `main`) и pull request.
- `pages` — на push в `main` (и вручную) собирает прошивку и публикует страницу установки: `site/`,
  прошивку в `fw/` и `manifest.json` для страницы.

## Подключение

Интерфейс MQTT-SN, имя и адрес узла задаёт раздел `cfg` ([docs/cfg-format.md](docs/cfg-format.md)), его
записывает страница установки. Без `cfg` или с испорченным `cfg` MQTT-SN идёт через UART0 — USB-порт
devkit: TX = GPIO1, RX = GPIO3, 38400 8N1. Пока поддерживается один интерфейс UART с кадром SLIP; WiFi,
RS-485 и интерфейсы-шлюзы — впереди ([TODO.md](TODO.md)), `cfg` с ними применяется как испорченный.

Для разработки `cfg` собирает и `tools/mkcfg.py`:

```
python tools/mkcfg.py --name kitchen --uart 1,17,16,-,38400     # порт, TX, RX, DE ("-" — нет), скорость
python -m esptool --port COM3 write-flash 0x1a0000 cfg.bin
```

Узел применяет `cfg` при запуске. Имя и адреса, записанные потом по сети, главнее `cfg`, пока не будет
записан новый `cfg`.

## Проверка с симулятором

```
pip install pyserial
python tools/gw_sim.py --selftest
python tools/gw_sim.py COM3          # USB devkit (UART0); отладочная сборка — адаптер на GPIO17/16
```

На чистом узле: DHCPREQ → SEARCHGW → CONNECT `E1S10_10` → REGISTER `pa1` → PUBLISH 0xFF07 (часы),
0xFFC0 `E1S10.400`, 0xFFC1 → SUBSCRIBE `#` → симулятор, как Enviriot, выставляет часы узла → состояние PLC.
Команды: `name kitchen` — имя узла,
`plc start|stop|crc`, `plc upload <файл.bin> [дно стека]` — загрузить программу, `reg 50 Md0` — мерке́р.

## Тесты

- Хостовые, на ПК без ESP-IDF: `python test/host/run.py` ([test/host](test/host/README.md)).
- Модули страницы установки (`cfg`, MD5, описание приложения, тексты): `node test/js/test_site.mjs`.
- Программы PLC для ручной проверки на устройстве с Enviriot — [tests/plc](tests/plc/README.md).
- Ввод-вывод на плате, образы без компилятора Enviriot — [tests/io](tests/io/README.md).

## План и история

[TODO.md](TODO.md) — что предстоит, [changelog.md](changelog.md) — что и почему менялось.

## Лицензия

MIT (`LICENSE`). Код, перенесённый из X13.Home, — BSD 3-Clause (`LICENSE.X13`).
