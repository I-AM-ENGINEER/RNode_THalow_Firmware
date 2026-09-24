# BLE: полный разбор, корневые причины нестабильности и переработка (ветка BLE-rework)

Дата: 2026-09-24. Все выводы ниже проверены по исходникам NimBLE в составе
ESP-IDF v5.4.4 (`C:\esp-idf\components\bt\host\nimble\...`), по исходникам
клиентов (columba, Reticulum) и по документации Espressif/Nordic/Apple.
Ссылки на источники — в конце документа.

---

## 1. Симптомы

1. После перезагрузки ESP32 пейринг (бонд) не восстанавливается.
2. Устройство **пропадает из saved devices на телефоне** — Android сам
   удаляет бонд, требуется пейриться заново.
3. columba (https://github.com/torlando-tech/columba) постоянно пишет
   дисконнекты.
4. После перезагрузки ESP32 работает нестабильно в целом.

## 2. Как всё устроено: путь данных

```
Телефон/ПК (columba / RNS RNodeInterface)
   │  BLE, Nordic UART Service (NUS), KISS-фреймы
   ▼
src/ble.c  ── RX: StreamBuffer ──► ble_rx_task ──► kiss.c (дефрейминг)
   ▲                                              │ payload ≤ 1024
   │ TX: tx_buf + flush_task (10 мс)              ▼
   │                                    switch.c (3 кольца, PSRAM)
   └── kiss_send_data ◄── ble_sink ◄──────────────┴──► halow_sink ──► SLIP ──► радио
```

- NUS: сервис `6e400001-…`, RX-характеристика `6e400002-…` (клиент пишет в
  устройство), TX `6e400003-…` (устройство нотифицирует). Это же используют
  официальный RNode и оба клиента.
- Поверх NUS — KISS-фрейминг (`src/kiss.c`), внутри — пакеты Reticulum.
- Центральный коммутатор `src/switch.c`: BLE- и TCP-кольца «перезаписывают
  старейший» при переполнении, HaLow-кольцо не теряет ничего (backpressure).

## 3. Корневая причина №1 (главная): ключи при пейринге не распределялись вообще

Старый код (`src/ble.c` до переработки):

```c
ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;   /* «LTK достаточно» */
```

Это фундаментальная ошибка, и вот почему — по исходникам NimBLE:

1. **При Secure Connections (SC) бит ENC вырезается из key distribution**
   (`ble_sm.c`, `ble_sm_key_dist()`):
   ```c
   /* Encryption info and master ID are only sent in legacy pairing. */
   if (proc->flags & BLE_SM_PROC_F_SC) {
       *out_init_key_dist &= ~BLE_SM_PAIR_KEY_DIST_ENC;
       *out_resp_key_dist &= ~BLE_SM_PAIR_KEY_DIST_ENC;
   }
   ```
   В SC LTK не передаётся — он вычисляется на обеих сторонах. Поэтому маска
   `ENC` при `sm_sc=1` превращается в **ноль**: не распределяется ничего.
2. Респондент маскирует запрос инициатора своим конфигом (`ble_sm.c`,
   `ble_sm_pair_rsp_fill()`):
   ```c
   rsp->init_key_dist = req->init_key_dist & ble_hs_cfg.sm_their_key_dist;
   ```
   То есть мы прямо говорили телефону: «свои IRK и identity-адрес не присылай».
3. Ключи сохраняются по адресу пира (`ble_sm_persist_keys()`): если identity
   не получен — бонд записывается **по RPA телефона в момент пейринга**
   (`conn->bhc_peer_addr`), а `irk_present=0` → в resolving list контроллера
   ничего не попадает (`ble_store.c: ble_store_write_peer_sec()` вызывает
   `ble_hs_pvcy_add_entry()` только при `irk_present`).
4. **Резолвить RPA без IRK пира математически невозможно.** Android и iOS
   подключаются к периферии через RPA (resolvable private address), который
   ротируется примерно каждые 15 минут и гарантированно меняется после
   перезагрузки телефона/ESP32.

### Цепочка отказа (полностью объясняет все симптомы)

```
пейринг (в одной RPA-сессии — работает)
  → ESP32 перезагрузка или ротация RPA телефона
  → телефон подключается с НОВЫМ RPA
  → resolving list пуст → NimBLE не находит LTK для «неизвестного» адреса
  → LL Encryption Start отклонён (PIN or Key Missing, 0x06)
  → Android считает бонд битым: удаляет его и пытается перепейриться
  → BLE_GAP_EVENT_REPEAT_PAIRING → прошивка удаляет свой бонд, RETRY
  → новый SMP требует passkey → allow_pairing=false (окно закрыто)
  → ble_gap_terminate(BLE_ERR_NO_PAIRING) — обрыв связи НА ЭТАПЕ ПЕЙРИНГА
  → Android удаляет бонд окончательно → «устройство пропало из saved devices»
  → цикл: connect → encrypt fail → disconnect → connect…  = «постоянные дисконнекты»
```

Эта же картина массово описана в багах Meshtastic/esp-nimble
(espnimble#16 «ESP32/nimble forgets BLE pairing connection to Android»,
esp-idf#16053).

### Исправление

```c
ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
```

Теперь бонд хранится по **identity-адресу** телефона, его IRK попадает в
resolving list контроллера, и при переподключении с любым новым RPA
контроллер сам резолвит адрес → LTK находится → шифрование восстанавливается
без повторного пейринга. Бонды лежат в NVS (`CONFIG_BT_NIMBLE_NVS_PERSIST=y`,
namespace `nimble_bond`), переживают перезагрузку, а `ble_hs_sync()` на
каждом старте host'а перезаливает IRK в контроллер
(`ble_hs_misc_restore_irks()`).

Комментарий в старом коде («IRK не шлём — был спам hci_err=0x212») лечится
отдельно, см. §5.3: 0x212 возникал не от самого факта IRK, а от повторного
добавления уже существующего identity в resolving list (в IDF 5.4.4 нет
upstream-фикса remove-before-add).

## 4. Корневая причина №2: MITM-passkey, который не защищал, но ломал клиентов

Старая модель: `sm_io_cap = DISP_ONLY`, `sm_mitm = 1`, статический passkey
`123456`, доступ к характеристикам требовал `encrypted && authenticated`.

Проблемы:

1. **Статический публичный passkey не даёт MITM-защиты.** MITM-смысл passkey
   в его одноразовости; зашитый в прошивку код эквивалентен Just Works
   (Espressif прямо пишет это в примере bleprph). Т.е. реальная защита не
   снижается переходом на Just Works.
2. **python Reticulum (bleak) headless не умеет вводить passkey.** BlueZ
   требует KeyboardDisplay-агента; на сервере без агента пейринг падает
   («br-connection-profile-unavailable»). RNS никогда не вызывает pair() и
   не обрабатывает passkey — он требует ГОТОВЫЙ бонд на уровне ОС.
3. **columba при Just Works подтверждает пейринг молча**
   (`BlePairingHandler`: PAIRING_VARIANT_CONSENT → `setPairingConfirmation(true)`),
   а PIN-вариант требует код, который приложению неоткуда взять (у нас нет
   USB-KISS канала 0x62, как у оригинального RNode).
4. Обрыв связи при `PASSKEY_ACTION` вне окна пейринга — то самое действие,
   из-за которого Android удаляет бонд из saved devices (см. §3).

### Новая модель (максимум совместимости при сохранении реальной защиты)

- **Just Works + Bonding + SC**: `sm_io_cap = NO_IO`, `sm_mitm = 0`,
  `sm_sc = 1`, `sm_bonding = 1`. Пейринг проходит без единого диалога на всех
  платформах: Android (columba auto-confirm / системный consent),
  Linux (`bluetoothctl pair` без агента), Windows.
- **Данные только по зашифрованной линии**: запись в RX-характеристику
  возвращает `BLE_ATT_ERR_INSUFFICIENT_AUTHEN`, пока ссылка не зашифрована;
  нотификации TX не отправляются до шифрования. Пассивное прослушивание
  исключено, обе стороны обязаны иметь бонд.
- **Жёсткий гейт пейринга (требование владельца: «всегда в pairing быть не
  должно») — на уровне Link Layer**: вне 35-секундного окна (кнопка BOOT)
  connect-реклама фильтруется whitelist'ом контроллера, который строится из
  бондов в NVS (`ble_store_util_bonded_peers` → `ble_gap_wl_set`,
  filter policy `BLE_HCI_ADV_FILT_SCAN`). Посторонний не может даже
  инициировать соединение — SMP не начинается, зомби-бонды не возникают.
  Бонус: вне окна чужие сканеры не получают и scan response — для них
  устройство «безымянное» (визард columba показывает только именованные
  RNode). Забонденные телефоны (RPA резолвится через resolving list по их
  IRK) подключаются всегда без кнопки — это нормальное поведение.
- Терминология честности: защита от активного MITM в момент первого пейринга
  отсутствовала и раньше (публичный 123456); от пассивного прослушивания
  защищаемся шифрованием; произвольный прохожий забондиться больше не может —
  только в окно пейринга.

## 5. Прочие найденные дефекты и их исправления

### 5.1. Silent-обрыв KISS-фреймов (RX и TX)

- `ble_write()` при переполнении `tx_buf` молча обрезал кадр посередине
  (`break`) и возвращал `len`, будто всё доставлено. KISS не имеет CRC —
  клиент получает битый фрейм → десинхронизация парсера → дисконнекты.
- RX-путь: `xStreamBufferSend(..., 0)` при полном стриме ронял хвост фрейма.

**Исправление:** политика «фрейм целиком или никак». `ble_write` либо
помещает весь фрейм, либо дропает весь новый фрейм (счётчик+лог). RX при
переполнении переходит в режим resync: байты отбрасываются до следующего
FEND (0xC0) — парсер KISS восстанавливается на границе фрейма.

### 5.2. Одновременные LL-процедуры после шифрования

Старый код на `ENC_CHANGE` запускал сразу и PHY-update (2M), и connection
parameter update. Две LL-процедуры подряд — классическая причина procedure
collision на Android-централах (исторический источник пост-пейринг
дисконнектов; именно поэтому их уже переносили с CONNECT на ENC_CHANGE).

**Исправление — последовательный запуск:** param update на `ENC_CHANGE`,
PHY 2M — только после завершения обновления параметров (`CONN_UPDATE`).
Одна процедура в момент времени. (Android игнорирует/отклоняет param update
раньше ~5 с от коннекта — это норма и не роняет линк.)

### 5.3. 0x212 «LE Add Device To Resolving List» при повторном пейринге

В IDF 5.4.4 `ble_store_write_peer_sec()` безусловно делает
`ble_hs_pvcy_add_entry()`. Если identity уже в resolving list (повторный
пейринг того же телефона), контроллер отвечает Invalid Parameters (0x212),
ошибка всплывает в SMP и пейринг падает. Upstream уже содержит фикс
(remove-then-add), в 5.4.4 его нет.

**Исправление:** в обработчике `REPEAT_PAIRING` после
`ble_store_util_delete_peer()` дополнительно вызывается
`ble_hs_pvcy_remove_entry()` (внутренняя функция NimBLE, стабильная
сигнатура; объявляем прототип явно). Re-add при RETRY проходит чисто.

### 5.4. KISS off-by-one: 1024-байтовый пакет терял последний байт

`rx_frame[KISS_FRAME_MAX]` вмещал байт команды + payload ⇒ максимум 1023
байт payload, а `RNS_FRAMING_MAX_PACKET = 1024`. Задокументировано даже в
тесте. **Исправление:** `KISS_FRAME_MAX = RNS_FRAMING_MAX_PACKET + 8`
(1024 + команда + запас), при переполнении — громкий отказ фрейма вместо
тихой обрезки + resync по FEND.

### 5.5. 2 КБ стек-буфер в 4 КБ задаче

`send_kiss()` собирал фрейм в стековом буфере 2056 байт и мог вызываться из
`ble_rx_task` (стек 4096, путь echo radio-config). **Исправление:**
статический scratch под мьютексом (вызовы возможны из двух задач).

### 5.6. Медленная реклама 1.3–1.6 с

bleak ищет цель сканом по 2 с (цикл 1 с); интервал рекламы 1.5 с — на грани
обнаружения, плюс медленный direct-connect у Android. **Исправление:**
медленная фаза = 160–250 мс (в 5–8 раз быстрее обнаружение, экономия
энергии ~3–4× относительно быстрой фазы сохраняется).

### 5.7. Marginal-настройки стека

- `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` 4096 — известные крэши host-задачи
  на границе; у нас в `gatt_access_cb` был стековый буфер 512 Б. → 5120, а
  буфер убран (mbuf → static scratch без большой локали).
- `MSYS_1_BLOCK_COUNT=12` — всего ~12 нотификаций в полёте (mbuf 256 Б на
  250-байтовый chunk). → 24/32.
- Мёртвый ключ `CONFIG_BT_NIMBLE_MAX_ATT_MTU` в sdkconfig.defaults (в 5.4
  не существует) — заменён на корректный набор.
- GAP PPCP (Peripheral Preferred Connection Parameters) отсутствовал →
  добавлен (40–80 мс, supervision 3 с) — подсказка централам, как у
  оригинального RNode (`setMinPreferred(0x20)/setMaxPreferred(0x40)`).

### 5.8. Нотификации до шифрования

CCCD открыт, случайный прохожий мог подписаться и получать радио-трафик по
незашифрованной линии. **Исправление:** flush не отправляет ничего, пока
`sec_state.encrypted` ложен.

## 6. Требования клиентов (проверено по их исходникам)

| Требование | columba (Kotlin) | RNS RNodeInterface (bleak) |
|---|---|---|
| Имя начинается с `"RNode "` (с пробелом) | ScanFilter + startsWith | startsWith (auto-режим) |
| NUS UUID в payload рекламы | setServiceUuid фильтр | adv.service_uuids фильтр |
| Бонд на уровне ОС | подключается ТОЛЬКО к bonded | фильтр Bonded==True |
| Write With Response | да (WRITE_TYPE_DEFAULT) | — |
| Write Without Response | — | да (`response=False`) |
| MTU | requestMtu(512), таймаут 2 с | авто (BlueZ/WinRT) |
| Нотификации | произвольный размер | произвольный размер |
| Разрыв фрейма | — | >1.25 с между кусками = сброс фрейма |
| FW version ≥ 1.52, echo radio-config, detect 0x73→0x46 | да (валидирует, иначе abort) | да (иначе RNS.panic) |

Всё это новая реализация выполняет. RX-характеристика объявлена с обоими
флагами записи (WRITE | WRITE_NO_RSP) — прошлый код уже так делал, сохранено.

## 7. Архитектура нового ble.c

```
Задачи:
  NimBLE host task (5120)   — все GAP/GATT колбэки, SMP
  ble_flush (4096, 10 мс)   — выталкивание tx_buf нотификациями
  ble_rx_task (4096)        — по-байтно в kiss_rx_byte (существующий)

Состояния: OFF → ON → PAIRING (окно, UX) → CONNECTED (шифрование установлено)

ADV: [fast 30 мс × 30 c] → [slow 160–250 мс ∞]; перезапуск на disconnect /
connect-fail / ADV_COMPLETE; fast ре-арм на disconnect и pairing-enable.
Пауза для WiFi-скана — счётчик глубины (ble_pause/ble_resume).

SMP: Just Works + Bonding + SC, keydist ENC|ID в обе стороны,
NVS persist, REPEAT_PAIRING = delete peer + remove resolving entry + RETRY.
Гейт: вне окна пейринга filter policy = whitelist бондов (посторонние
не подключаются и не видят имя); в окне — реклама без фильтра.
Закрытие окна (таймаут/успех) переключает рекламу обратно в гейт-режим.

LL-процедуры: ENC_CHANGE → param update (40–80 мс); CONN_UPDATE → PHY 2M.
Больше никакой одновременности.

TX: tx_buf 2064 Б, приём фрейма целиком или дроп целиком (счётчики);
     chunk = min(MTU-3, 509); при BLE_HS_ENOMEM — повтор через 10 мс.
RX: mbuf → static scratch → StreamBuffer 2048; при переполнении resync по FEND.
```

## 8. Как проверить (acceptance)

1. `make -C tests test` — host-тесты (теперь компилируют настоящий kiss.c).
2. `python tests/qemu/run_qemu.py` — QEMU esp32s3.
3. Прошивка на COM6 + `python -m pytest tests/hw -v` — смок + реклама.
4. Ручной сценарий телефона: пейринг (System Settings / columba wizard) →
   перезагрузка ESP32 (`esp_restart` кнопкой/питанием) → телефон
   переподключается БЕЗ повторного пейринга, бонд остаётся в saved devices;
   выключить/включить Bluetooth на телефоне → переподключение автоматом.
5. python RNS: `bluetoothctl pair <MAC>` (без агента, Just Works молча),
   затем интерфейс `port = ble://` / `ble://RNode HaLow XXXXXX` в конфиге
   Reticulum, рестарт, `rnsdump`/announce — трафик идёт.

## 9. Возможные будущие улучшения (не входят в эту ветку)

- Статический passkey через `ble_sm_configure_static_passkey()` — если
  понадобится MITM-режим с отображаемым на web-дашборде кодом.
- Directed advertising к забонденному центру для ускорения реконнекта.
- RSSI/SNR-отчёты (KISS 0x23/0x24) перед датафреймами, как у оригинального
   RNode — RNS их парсит, но не требует.

## 10. Источники

- NimBLE (espressif fork): `ble_sm.c` (key_dist/SC-стриппинг, persist_keys),
  `ble_store.c`, `ble_hs_pvcy.c`, `ble_hs_startup.c` (генерация local IRK),
  `ble_gattc.c` (notify_custom/NOTIFY_TX) — локально в IDF 5.4.4 и
  github.com/espressif/esp-nimble.
- esp-nimble issue #16 (забывается бонд к Android), esp-idf issue #16053
  (0x212 resolving list).
- Клиенты: github.com/torlando-tech/columba (BluetoothLeConnection.kt,
  KotlinRNodeBridge.kt, BlePairingHandler.kt, rnode_interface.py),
  github.com/markqvist/Reticulum (RNS/Interfaces/RNodeInterface.py).
- Оригинальный RNode: github.com/markqvist/RNode_Firmware (BLESerial.cpp,
  Bluetooth.h) — Bluedroid-стек, ENC|ID в обе стороны, окно пейринга 35 с.
- Espressif coexist guide; Apple QA1931 (connection parameters);
  Punch Through «BLE connection parameters»/«maximizing throughput»;
  Nordic DevZone (RPA разрешаются только IRK; iOS MTU 185);
  Google issuetracker 288211591 (Android MTU), 315187773 (SC passkey bug).
