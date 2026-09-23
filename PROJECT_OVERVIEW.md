# ZET 017 — Обзор проекта

> **Назначение:** Кроссплатформенная C-библиотека для управления измерительными устройствами ZET (ZET 017 / ZET 058 и др.) по TCP, с модульным парсером XML-конфигурации.

---

## 1. Общее назначение

Проект представляет собой набор DLL/библиотек для интеграции аппаратуры ZET с клиентскими приложениями, в первую очередь с **LabView**. Библиотеки обеспечивают:

- **TCP-соединение** с устройством по трём портам (команды, АЦП, ЦАП).
- **Чтение/запись конфигурации** устройства.
- **Потоковый сбор данных** с аналого-цифровых каналов.
- **Вывод данных** на цифро-аналоговые каналы.
- **Разбор и запись XML-конфигурации** (`devices.cfg`) — вынесен в отдельный модуль.

---

## 2. Архитектура

Проект разделён на **два независимых модуля**, чтобы основная библиотека не тащила зависимость от `libxml2`.

```
┌─────────────────────────────────────────────────────────────┐
│                      Клиент (LabView)                        │
│  ┌─────────────────┐      ┌─────────────────────────────┐   │
│  │  zet017tcp.dll  │◄────►│      zet017config.dll       │   │
│  │  (TCP + ядро)   │      │   (только XML read/write)   │   │
│  └─────────────────┘      └─────────────────────────────┘   │
│         ▲                            ▲                      │
│         │                            │                      │
│         └────────────┬───────────────┘                      │
│                      │                                      │
│              Бинарно идентичные структуры                   │
│        (zet017_config / zet017_tenso_config)                │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│              Устройство ZET (по Ethernet)                    │
│         Порты: 1808 (команды), 2320 (АЦП), 3344 (ЦАП)       │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. Структура директории

```
zet017-native-sdk/
├── README.md                 # Подробная документация по сборке и LabView
├── example_usage.c           # Пример кода: чтение XML → конфигурация устройства
│                             #   и обратно: снятие конфигурации → запись в XML
├── devices.cfg               # Пример файла конфигурации ZETLAB (XML, version="1.2")
│
├── zet017tcp/                # ⬅ ОСНОВНАЯ библиотека (НЕТ внешних зависимостей)
│   ├── include/zet017tcp.h   #   Публичный API + структуры данных
│   ├── src/zet017tcp.c       #   Реализация: сокеты, потоки, протокол устройства
│   ├── win32/zet017tcp.def   #   Экспорт символов для Windows DLL
│   ├── CMakeLists.txt        #   Сборка без vcpkg/libxml2
│   └── CMakePresets.json     #   Пресеты CMake (vs-x86, vs-x64)
│
└── zet017config/             # ⬅ ОТДЕЛЬНЫЙ модуль XML (зависит от libxml2)
    ├── include/zet017config.h#   Публичный API: load_file / save_file
    ├── src/zet017config.c    #   Реализация на libxml2: чтение/запись devices.cfg
    ├── win32/zet017config.def#   Экспорт символов для Windows DLL
    ├── CMakeLists.txt        #   find_package(LibXml2)
    ├── CMakePresets.json     #   Пресеты с vcpkg manifest-режимом
    └── vcpkg.json            #   Зависимость: libxml2
```

---

## 4. Основная библиотека: `zet017tcp`

### 4.1. Публичный API (`zet017tcp.h`)

| Функция | Назначение |
|---------|-----------|
| `zet017_server_create` / `free` | Создание/уничтожение сервера (контекста соединения) |
| `zet017_server_add_device` / `remove_device` | Добавление/удаление устройства по IP |
| `zet017_device_get_info` | Информация об устройстве (IP, имя, серийный номер, версия прошивки) |
| `zet017_device_get_state` | Состояние подключения, указатели буферов АЦП/ЦАП |
| `zet017_device_get_config` | Текущая конфигурация устройства (частоты, каналы, усиление, генератор) |
| `zet017_device_get_tenso_config` | Конфигурация тензодатчиков (схема моста, коррекция) |
| `zet017_device_set_config` / `set_tenso_config` | Отправка новой конфигурации на устройство |
| `zet017_device_start` / `stop` | Запуск/остановка сбора АЦП (и опционально ЦАП) |
| `zet017_channel_get_data` | Чтение float-массива с канала АЦП (в вольтах) |
| `zet017_channel_put_data` | Запись float-массива на канал ЦАП (в вольтах) |

### 4.2. Структуры данных

```c
struct zet017_config {
    uint32_t sample_rate_adc;      // Частота АЦП (Гц): 50000, 25000, 5000, 2500
    uint16_t moda_adc;             // Код режима АЦП (1..4)
    uint32_t sample_rate_dac;      // Частота ЦАП (вычисляется: 80 МГц / rate_dac)
    uint16_t rate_dac;             // Код делителя частоты ЦАП
    uint32_t mask_channel_adc;     // Битовая маска активных каналов АЦП
    uint32_t mask_icp;             // Битовая маска ICP-каналов
    uint32_t gain[8];              // Коэффициент усиления (1, 10, 100)
    uint16_t gain_code[8];         // Сырой код усиления (0, 1, 2)
    uint16_t builtin_dac_state;    // Биты: встроенный генератор, синус
    double builtin_dac_sine_freq;  // Частота синуса (Гц)
    double builtin_dac_sine_ampl;  // Амплитуда синуса
    double builtin_dac_sine_offset;// Смещение синуса
};

struct zet017_tenso_config {
    enum zet017_scheme scheme[8];  // Схема моста: bridge / half_bridge / quarter_bridge
    uint8_t correction_1[8];       // Значение коррекции Pot1
    uint8_t correction_2[8];       // Значение коррекции Pot2
};
```

### 4.3. Ключевые особенности реализации

- **Кроссплатформенность:** единый код для Windows (`WinSock2`, `CriticalSection`) и POSIX (`pthread`, ` Berkeley sockets`).
- **Асинхронная сеть:** неблокирующие сокеты + `select()` с таймаутами.
- **Потоки:** каждое устройство обслуживается в отдельном рабочем потоке.
- **Безопасность при сбоях:** файл конфигурации заменяется атомарно через временный; при ошибке исходный файл остаётся неизменным.
- **LabView-совместимость:**
  - Заголовки **самодостаточны** — не требуют `<stdint.h>` (ручные `typedef` через `__int32`).
  - Соглашение вызова `__stdcall` (`WINAPI`) для всех экспортируемых функций.
  - `float*` параметры передаются как **Array Data Pointer** в LabView.
  - Статический рантайм MSVC (`/MT`) — DLL не требует `VCRUNTIME140.dll`.

---

## 5. Модуль XML: `zet017config`

### 5.1. Назначение

Изолированная DLL, единственная задача которой — **читать и писать** XML-файл `devices.cfg` (формат ZETLAB). Она **не общается с устройством** — это делает `zet017tcp`.

### 5.2. Публичный API (`zet017config.h`)

| Функция | Назначение |
|---------|-----------|
| `zet017_config_load_file(path, serial, &config, &tenso)` | Читает XML и накладывает значения на структуры |
| `zet017_config_save_file(dir, file, serial, &config, &tenso)` | Записывает структуры обратно в существующий XML-файл |

### 5.3. Особенности записи

- **Файл-донор обязателен:** модуль **не создаёт** файл с нуля, а обновляет существующий. Причина — реальный `devices.cfg` содержит ~30 элементов на устройство и ~16 на канал, которые приходят от устройства во внутренней служебной структуре и недоступны через публичный API.
- **Атомарная замена:** запись идёт во временный файл `.tmp` рядом с оригиналом, затем выполняется `rename`.
- **Сохранение оформления:** восстанавливаются CRLF-переводы строк (если файл был в Windows-стиле) и пробел в пустых тегах (`<AFCH />`).
- **Кодировка:** явно добавляется `encoding="UTF-8"`, чтобы libxml2 не заменял кириллицу (например, `№`) на XML-ссылки (`&#x2116;`).

### 5.4. Приоритет значений

При чтении и записи действует тот же приоритет, что и в `zet017_device_set_config`:

| Поле структуры | Приоритет | Что попадает в файл/устройство |
|----------------|-----------|-------------------------------|
| `sample_rate_adc != 0` | Выше | `ModaADC = код частоты`, `Freq = sample_rate_adc` |
| `sample_rate_adc == 0` | Ниже | Берётся `moda_adc` как сырой код |
| `gain[i] != 0` | Выше | `KodAmplify[i] = код усиления` |
| `gain[i] == 0` | Ниже | Берётся `gain_code[i]` как сырой код |

---

## 6. Пример рабочего цикла

### 6.1. Настроить устройство из XML-файла

```c
// 1. Получить текущую (заводскую) конфигурацию с устройства
zet017_device_get_config(handle, dev_num, &config);
zet017_device_get_tenso_config(handle, dev_num, &tenso_config);

// 2. Наложить значения из XML (структуры бинарно идентичны)
zet017_config_load_file("C:\\ZETLAB\\devices.cfg", serial,
    (struct zet017_cfg_config*)&config,
    (struct zet017_cfg_tenso_config*)&tenso_config);

// 3. Отправить на устройство
zet017_device_set_config(handle, dev_num, &config);
zet017_device_set_tenso_config(handle, dev_num, &tenso_config);
```

### 6.2. Сохранить текущие настройки в XML

```c
// 1. Снять текущее состояние с устройства
zet017_device_get_config(handle, dev_num, &config);
zet017_device_get_tenso_config(handle, dev_num, &tenso_config);
zet017_device_get_info(handle, dev_num, &info);

// 2. Записать в файл
zet017_config_save_file("C:\\ZETLAB", NULL, info.serial,
    (const struct zet017_cfg_config*)&config,
    (const struct zet017_cfg_tenso_config*)&tenso_config);
```

---

## 7. Сборка

### 7.1. Требования

- **CMake** ≥ 3.15
- **MSVC** (Visual Studio) для Windows-сборки
- **vcpkg** — только для `zet017config` (устанавливает `libxml2` автоматически)

### 7.2. Команды

```bash
# --- Основная библиотека (vcpkg НЕ нужен) ---
cmake --preset vs-x86              # или vs-x64
cmake --build out/build/vs-x86

# --- XML-модуль (vcpkg установит libxml2) ---
cmake --preset vs-vcpkg-x86        # динамический рантайм
cmake --build out/build/vs-vcpkg-x86

# Для статического рантайма (/MT) — рекомендуется для LabView:
cmake --preset vs-vcpkg-x86-static
cmake --build out/build/vs-vcpkg-x86-static
```

---

## 8. Файлы конфигурации устройства

### `devices.cfg`

XML-файл формата ZETLAB (корневой элемент `<Config version="1.2">`). Содержит:

- **Глобальные параметры устройства:** `typeADC`, `Channel`, `ModaADC`, `Rate`, `KodAmplify`, `BuiltinGenSineFreq` и др.
- **Параметры каналов** (`<Channels>/<Channel id="N">`): `Sense`, `Amplify`, `Tenso`, `Pot1`, `Pot2`, `TensoIcp` и др.

Пример фрагмента:
```xml
<Device name="ZET 058 №711347" serial="711347" ...>
    <ModaADC>1</ModaADC>
    <Rate>160</Rate>
    <KodAmplify>2,1,1,0,0,0,0,0,...</KodAmplify>
    <Channels>
        <Channel id="0" name="ZET 058_712403_1" units="мВ">
            <Sense>0.0010000000474974513</Sense>
            <Tenso>0</Tenso>
            <Pot1>183</Pot1>
            <Pot2>183</Pot2>
        </Channel>
    </Channels>
</Device>
```

---

## 9. Подключение в LabView

1. **Import Shared Library Wizard:**
   - Include Paths — **пусто** (заголовки самодостаточны).
   - Preprocessor Definitions — **пусто**.
   - Calling Convention = **stdcall (WINAPI)**.

2. **Для массивов:** параметры `float* data` в `channel_get_data` / `channel_put_data` указываются как **Array of Single → Array Data Pointer**.

3. **Разрядность:** DLL должна совпадать с разрядностью LabView (обычно 32-бит → x86).

4. **Распространение:**
   - Всегда нужна: `zet017tcp.dll` (нет зависимостей).
   - Если нужен XML: дополнительно `zet017config.dll` + `libxml2.dll` (+ `zlib`, `iconv`, `lzma` при необходимости).

---

## 10. Лицензия и авторство

- Работа на основе библиотеки [zetlab/zet017tcp](https://github.com/zetlab/zet017tcp),
  не форк репозитория: структура каталогов изменена, добавлен модуль
  `zet017config`.
- Лицензия та же, что у оригинала, — **MIT**, см. файл `COPYING`
  (`Copyright (c) 2023 ZETLAB`). Условие MIT — сохранять текст лицензии и
  указание авторства во всех копиях и производных работах.
- Перечень отличий от оригинала — в разделе «Происхождение и лицензия»
  файла `README.md`.
- Реализация протокола TCP поверх Ethernet для линейки устройств ZET.
