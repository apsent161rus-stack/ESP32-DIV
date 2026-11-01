# Локальная сборка (Windows) — рабочий рецепт

Проверено на ESP32-DIV v2 (ESP32-S3, 16 МБ flash, 8 МБ OPI PSRAM), плата определяется
по нативному USB как `USB JTAG/serial debug unit`.

## Единственно верные версии

| Компонент | Версия | Почему именно эта |
|---|---|---|
| ESP32 core (`esp32:esp32`) | **2.0.10** | В `CONTRIBUTING.md` указано «2.0.10 exactly». На 2.0.17 линковка падает: `multiple definition of ieee80211_raw_frame_sanity_check` и `sspi`. На 3.x не собирается вовсе (`esp_event_loop.h` удалён) |
| NimBLE-Arduino | **1.4.3** | В 1.4.1 нет `getPayloadByType()`; в 2.x сломали старый HID-API (`inputReport`, `manufacturer`, `pnp`), который нужен `ducky.cpp` |
| arduinoFFT | **1.6.2** | В 2.0 класс переименован `arduinoFFT` → `ArduinoFFT`, ломается `subghz.cpp` |
| PCF8574 | **xreef 2.4.0** (`PCF8574_library`) | Только у неё есть `pinMode()` и `digitalRead()`. RobTillaart и реестровая 0.3.9 их не имеют |
| TFT_eSPI | из `Libraries/TFT_eSPI-master.zip` | Кастомная копия проекта. `User_Setup v2.h` → `User_Setup.h` |
| SmartRC-CC1101 | из `Libraries/SmartRC-CC1101-Driver-Lib-master.zip` | В архиве ДВА драйвера (`SRC_DRV` и `SRC_JT_DRV`) с одинаковыми символами — JT-вариант нужно убрать из корня, иначе `multiple definition of ELECHOUSE_CC1101::SpiStart()` |
| ArduinoJson / RF24 / rc-switch / IRremoteESP8266 / Adafruit PN532 + BusIO / XPT2046_Touchscreen | актуальные | Совместимы |

## Обязательная подмена platform.txt

`Libraries/platform.txt` из репозитория нужно **скопировать поверх** штатного
`%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\2.0.10\platform.txt`
(сделать бэкап `platform.txt.orig`). Отличия от штатного:

- `compiler.c.elf.libs.*` содержит флаг **`-zmuldefs`** — без него линковка падает с
  `multiple definition of ieee80211_raw_frame_sanity_check` (переопределение в `wifi.cpp`)
  и `multiple definition of spi`;
- `build.extra_flags.esp32s3` содержит **`-w`** (подавление предупреждений).

Без этой подмены сборка не проходит ни на 2.0.10, ни на 2.0.17.

## FQBN — рабочий (проверено на живой плате)

```
esp32:esp32:esp32s3:FlashSize=4M,PSRAM=disabled,PartitionScheme=no_ota,CDCOnBoot=default
```

Размер скетча при этих флагах: **1 745 153 байт (83% от 2 МБ)** — против 1 743 424 у официального
префаба v1.7.2, то есть конфигурация совпадает с авторской.

### Почему НЕ другие варианты

| Вариант | Симптом |
|---|---|
| `FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc` | Сборка проходит, но плата даёт **белый экран** — прошивка падает до `tft.init()`, судя по всему на инициализации PSRAM |
| ESP32 core 2.0.17 / 3.x | Линковка или компиляция падают (см. выше) |

Если понадобятся большие буферы в PSRAM — сначала отдельным тестом проверить `PSRAM=enabled`
(QSPI), не `opi`.

### Первичная проверка после прошивки

Официальный префаб (`Pre-compiled Bin/ESP32-DIV-v2-v1.7.2.bin` на `0x10000`) — быстрый способ
отделить «сломанную сборку» от «проблем с железом»: если он показывает меню, железо и дисплей живы.

## Сборка (старое описание FQBN для 3 МБ раздела оставлено ниже как история)

```
esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc
```

## Сборка

```bat
arduino-cli compile -b "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc" ^
  --output-dir build ESP32-DIV
```

## Прошивка

```bat
:: вся прошивка (bootloader + разделы + приложение)
esptool --port COM4 write-flash 0x0 build\ESP32-DIV.ino.bootloader.bin ^
  0x8000 build\ESP32-DIV.ino.partitions.bin ^
  0x10000 build\ESP32-DIV.ino.bin

:: только приложение, если разметка разделов не менялась
esptool --port COM4 write-flash 0x10000 build\ESP32-DIV.ino.bin
```

## Грабли, на которых уже наступили

1. Кэш `%LOCALAPPDATA%\arduino\sketches\<hash>` переживает смену состава библиотек — старые `objs.a` дают ложные `multiple definition`. Чистить перед пересборкой после правок в библиотеках.
2. `pcf.digitalRead()` / `pcf.pinMode()` — не заменять на `read()`: у xreef API нет метода `read()`.
3. Правки в `subghz.cpp` и `ESP32-DIV.ino` от «универсализации под RobTillaart» откатаны — они ломают сборку с xreef.
