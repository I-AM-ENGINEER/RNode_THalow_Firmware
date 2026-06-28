# RNode-HaLow Firmware

Прошивка на базе **ESP-IDF** (FreeRTOS) для платы ESP32-S3-WROOM-1 N16R8.
Узел RNode: радиоинтерфейс на UART1 пробрасывается наружу по сети
(WiFi SoftAP, USB-CDC-NCM, SLIP).

Аппаратные детали — см. [`docs/hardware.md`](docs/hardware.md).

## Требования к окружению

- **ESP-IDF v5.x** для цели `esp32s3` (см. установку ниже).
- `IDF_PATH` указывает на клон `esp-idf`; тулчейн управляется через `export.ps1`.
- COM-порт платы — **COM6** (CH343 USB-UART).

## Установка ESP-IDF (Windows)

```powershell
# 1. Клонировать ESP-IDF (один раз, вне проекта)
git clone --recursive https://github.com/espressif/esp-idf.git C:\esp-idf
#   (или конкретный релиз: --branch v5.3.1 --recursive)

# 2. Установить тулчейн для esp32s3
cd C:\esp-idf
.\install.bat esp32s3

# 3. В каждой новой сессии — активировать окружение
C:\esp-idf\export.ps1
```

## Сборка и прошивка

```powershell
# активировать окружение ESP-IDF
. C:\esp-idf\export.ps1

# настроить цель
idf.py set-target esp32s3

# конфигурация (по необходимости)
idf.py menuconfig

# сборка + прошивка + монитор (COM6)
idf.py -p COM6 flash monitor
```

## Структура проекта

```
.
├── CMakeLists.txt          # корневой файл проекта ESP-IDF
├── sdkconfig.defaults      # конфиг по умолчанию (версонируется)
├── main/                   # точка входа приложения
├── components/             # переиспользуемые компоненты проекта
├── managed_components/     # от IDF Component Manager (НЕ коммитить)
├── docs/                   # документация
└── tools/                  # вспомогательные скрипты
```
