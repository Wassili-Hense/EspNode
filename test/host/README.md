# Хостовые тесты

Код компонентов собирается обычным компилятором для ПК, ESP-IDF не нужен: заглушки ESP-IDF — в `stubs/`,
железо ввода-вывода — поддельное (`fake_hw.c`).

```
python test/host/run.py                 # все
python test/host/run.py vm io           # выборочно
CC=clang python test/host/run.py        # компилятор: $CC, по умолчанию gcc
```

| Тест | Что проверяет |
|---|---|
| `vm` (`test_vm.c`) | VM PLC: команды, float, память, вызовы, `IN`/`OUT` |
| `io` (`test_io.c`) | Ввод-вывод: режимы и пины, ресурсы, шины, перезапуск программы; байткод через VM → API → `plc_io` |
| `cfg` (`test_cfg.c`) | Раздел `cfg`: пример из [docs/cfg-format.md](../../docs/cfg-format.md), ошибки заголовка и записей, проверка пинов и портов |
| `rs485` (`test_rs485.c`) | Кадр X13 RS-485: формат и CRC, свой адрес, 0 и чужой, мусор, плохие CRC и `LEN`, предельная длина |
| `cfg_gen` | Генератор `tools/mkcfg.py` воспроизводит тот же пример |
| `e2e` (`e2e.py`) | MQTT-SN: подключение, потеря и возврат шлюза, перезагрузка, новый `cfg` и смена типа — узел `host_node` против `tools/gw_sim.py` |
| `e2e_gw` (`e2e_gw.py`) | Шлюз: второй `host_node` (E1S10) за этим на PHY2 — DHCP, GWINFO, сессия и PUBLISH через FORWARD, ADVERTISE, чужой PHY, 50 байт, без своей сессии |
| `e2e_rs485` (`e2e_rs485.py`) | Шина RS-485: шлюз E1SM10 и два узла E1M10 (три `host_node`) — разные адреса по DHCP, GWINFO с адресом шлюза, PUBLISH одному узлу, ADVERTISE всем |
| `e2e_udp` (`e2e_udp.py`) | MQTT-SN по UDP, как WiFi в `main.c`: адреса IPv4, поиск шлюза, имя из MAC, PHY1 = IP, чужие адреса |
| `e2e_plc` (`e2e_plc.py`) | PLC через `pa1`: загрузка как в Enviriot, мерке́ры, отчёт об ошибке, сохранение, ввод-вывод при перезапуске |

`host_node` — узел целиком (MQTT-SN, Object Dictionary, PLC) с моделируемым временем; `harness.py` гоняет
его тиками по 10 мс и связывает с симулятором шлюза. Сборка и журналы узла — в `test/host/build/`.
