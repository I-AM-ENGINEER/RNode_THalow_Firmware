# RNode-THaLow ESP32

TWX8301 (HaLow module) Firmware: <https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware>

[English](#english) | [Русский](#русский)

---

## English

Firmware for the ESP32-S3-WROOM-1 that turns the board into a Bluetooth KISS
modem bridged to a WiFi HaLow radio over SLIP/TCP.

The ESP32 acts as a transparent bridge.

> **Compatibility:** The T-Halow board is supported by `rnodeconf` (the RNode
> flasher) starting from version **2.2.0b** and newer. Older versions will
> brick the HaLow module and require reflashing it with an external SPI
> programmer (CH341A).

### Features

- Transparent BLE <-> HaLow radio bridge
- Secure BLE pairing (passkey, bonding)

### Hardware

- **Board:** [LilyGO T-Halow](https://github.com/Xinyuan-LilyGO/T-Halow) — ESP32-S3-WROOM-1 N16R8 (16 MB flash, 8 MB PSRAM)
- **Radio:** WiFi HaLow module connected via SLIP link (2,000,000 baud)

### Prerequisites

The T-Halow board ships with stock firmware on the HaLow radio module. For
this firmware to work, the radio module's SPI flash must first be reflashed
with [RNode-HaLow](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware)
using an external SPI flash programmer (CH341A recommended). This is a
one-time step done before flashing the ESP32-S3.

### Status LED

| State        | LED behavior          |
|--------------|-----------------------|
| Off          | Off                   |
| Advertising  | Blink 500 ms          |
| Pairing      | Blink 100 ms (fast)   |
| Connected    | On (solid)            |

### Pairing

1. Make sure the device is powered on.
2. Press and hold the BOOT button for 3 seconds.
3. The LED starts blinking fast — the device is in pairing mode for 35 seconds.
4. Connect from your phone/PC; the device advertises as `RNode HaLow XXYYZZ`
   (last 3 bytes of the MAC address).
5. Enter passkey `123456` when prompted.

After pairing, the bond is saved and reconnection is automatic.

### Connecting

The device exposes a BLE KISS interface. Use any KISS-compatible client
(Columba, Sideband, etc.) to connect over BLE.

The HaLow radio module is reached via a SLIP link on UART1 at
`192.168.7.1` (radio at `192.168.7.2`), TCP port `4242`.

> **Note:** LoRa parameters in clients (Sideband, Columba, etc.) do not affect
> the HaLow radio. There is currently no way to configure the radio over BLE.
> To change the radio settings, connect an Ethernet cable to the RNode-HaLow
> and use its web control panel on the local network.

### Building and Flashing

Requires ESP-IDF v5.4.x.

```powershell
# Activate ESP-IDF environment
. C:\esp-idf\export.ps1

# Set target (first time only)
idf.py set-target esp32s3

# Build
idf.py build

# Flash and monitor (replace COM6 with your port)
idf.py -p COM6 flash monitor
```

### Configuration

Board-specific settings (pins, BLE name, UART baudrate, etc.) are in
`inc/config/project_config.h`.

### Project Structure

```
.
|-- CMakeLists.txt          # Top-level ESP-IDF project
|-- sdkconfig.defaults      # Default sdkconfig (version-controlled)
|-- partitions.csv          # Flash partition layout
|-- src/                    # Application source
|   |-- main.c              # app_main entry point
|   |-- system.c            # Transport binding, BLE<->KISS<->HaLow
|   |-- ble.c               # BLE NimBLE (NUS, pairing, advertising)
|   |-- kiss.c              # KISS protocol (instance-based)
|   |-- rns_framing.c       # HDLC framing (instance-based)
|   |-- slip.c              # SLIP netif to radio module
|   |-- status_led.c        # LED state indicator
|   `-- button.c            # BOOT button -> pairing events
|-- inc/                    # Project headers
|   `-- config/
|       `-- project_config.h
`-- libs/                   # Third-party libraries
```

### Roadmap

- [ ] In-app radio configuration (proper UI for setting RF parameters without Ethernet)
- [ ] BLE Mesh support
- [ ] Microreticulum (embedded RNS stack on-device)
- [ ] USB NCM interface
- [ ] ESP32-S3 power optimisations (deep sleep, wake-on-BLE)

### Development Notes

This firmware was developed with active assistance from the LLM
**GLM-5.1 / GLM-5.2** (Zhipu AI). If you have objections to AI-assisted
software, you may choose not to use this project.

---

## Русский

Прошивка для ESP32-S3-WROOM-1, превращающая плату в Bluetooth KISS-модем,
связанный с WiFi HaLow радиомодулем через SLIP/TCP.

ESP32 работает как прозрачный мост.

> **Совместимость:** Плата T-Halow поддерживается `rnodeconf` (прошивальщик
> RNode) начиная с версии **2.2.0b** и новее. Старые версии окирпичивают
> радиомодуль HaLow и требуют прошивки внешним SPI-программатором (CH341A).

### Возможности

- Прозрачный мост BLE <-> радиомодуль HaLow
- Безопасное BLE-сопряжение (passkey, bonding)

### Аппаратная часть

- **Плата:** [LilyGO T-Halow](https://github.com/Xinyuan-LilyGO/T-Halow) — ESP32-S3-WROOM-1 N16R8 (16 МБ флеш, 8 МБ PSRAM)
- **Радиомодуль:** WiFi HaLow, подключён через SLIP-канал (2 000 000 бод)

### Предварительные требования

Плата T-Halow поставляется со штатной прошивкой на радиомодуле HaLow. Для
работы данной прошивки необходимо предварительно прошить SPI-флеш радиомодуля
прошивкой [RNode-HaLow](https://github.com/I-AM-ENGINEER/RNode_Halow_Firmware)
с помощью внешнего программатора SPI-флеш (рекомендуется CH341A). Это
одноразовая процедура, выполняемая до прошивки ESP32-S3.

### Статусный светодиод

| Состояние    | Поведение светодиода   |
|--------------|------------------------|
| Выключено    | Выключен               |
| Реклама      | Мигание 500 мс         |
| Сопряжение   | Мигание 100 мс (быстро)|
| Подключено   | Горит (постоянно)      |

### Сопряжение

1. Убедитесь, что устройство включено.
2. Нажмите и удерживайте кнопку BOOT 3 секунды.
3. Светодиод начинает быстро мигать — устройство в режиме сопряжения 35 секунд.
4. Подключитесь с телефона/ПК; устройство называется `RNode HaLow XXYYZZ`
   (последние 3 байта MAC-адреса).
5. Введите passkey `123456` при запросе.

После сопряжения связь сохраняется, переподключение происходит автоматически.

### Подключение
Устройство предоставляет BLE KISS-интерфейс. Используйте любой
KISS-совместимый клиент (Columba, Sideband и т.п.) для подключения через BLE.

Радиомодуль HaLow доступен через SLIP-канал на UART1 по адресу
`192.168.7.1` (радиомодуль — `192.168.7.2`), TCP-порт `4242`.

> **Примечание:** Параметры LoRa в клиентах (Sideband, Columba и т.п.) не влияют
> на радиомодуль HaLow. На данный момент настроить радиомодуль через BLE нельзя.
> Для изменения настроек подключите Ethernet-кабель к RNode-HaLow и используйте
> веб-панель управления в локальной сети.

### Сборка и прошивка

Требуется ESP-IDF v5.4.x.

```powershell
# Активировать окружение ESP-IDF
. C:\esp-idf\export.ps1

# Установить таргет (первый раз)
idf.py set-target esp32s3

# Сборка
idf.py build

# Прошивка и монитор (замените COM6 на ваш порт)
idf.py -p COM6 flash monitor
```

### Конфигурация

Настройки платы (пины, имя BLE, скорость UART и т.д.) находятся в
`inc/config/project_config.h`.

### Структура проекта

```
.
|-- CMakeLists.txt          # Проект ESP-IDF верхнего уровня
|-- sdkconfig.defaults      # sdkconfig по умолчанию (под контролем git)
|-- partitions.csv          # Разметка флеш-памяти
|-- src/                    # Исходный код приложения
|   |-- main.c              # Точка входа app_main
|   |-- system.c            # Связывание транспорта, BLE<->KISS<->HaLow
|   |-- ble.c               # BLE NimBLE (NUS, сопряжение, реклама)
|   |-- kiss.c              # Протокол KISS (на экземплярах)
|   |-- rns_framing.c       # HDLC-фрейминг (на экземплярах)
|   |-- slip.c              # SLIP netif к радиомодулю
|   |-- status_led.c        # Индикатор состояния светодиода
|   `-- button.c            # Кнопка BOOT -> события сопряжения
|-- inc/                    # Заголовки проекта
|   `-- config/
|       `-- project_config.h
`-- libs/                   # Сторонние библиотеки
```

### План развития

- [ ] Настройка радиомодуля из приложения (нормальный UI для задания RF-параметров без Ethernet)
- [ ] Поддержка BLE Mesh
- [ ] Microreticulum (встроенный RNS-стек на устройстве)
- [ ] Интерфейс USB NCM
- [ ] Оптимизация энергопотребления ESP32-S3 (deep sleep, wake-on-BLE)

### Примечание о разработке

Данная прошивка разрабатывалась при активном участии LLM
**GLM-5.1 / GLM-5.2** (Zhipu AI). Если вы имеете что-то против ИИ-ассистированного
ПО, можете не использовать данное программное обеспечение.
