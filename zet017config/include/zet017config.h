#ifndef ZET017_CONFIG_H
#define ZET017_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Фиксированные целочисленные типы БЕЗ подключения <stdint.h>.
 *
 * Мастер импорта LabView (Import Shared Library) часто не может разобрать
 * системный stdint.h из современного MSVC (он не самодостаточен и требует
 * vcruntime.h/внутренние заголовки), из-за чего uint32_t остаётся
 * неопределённым. Поэтому нужные типы объявлены вручную — заголовок
 * становится самодостаточным, и в мастере НЕ нужно указывать ни одного
 * пути к заголовкам и ни одной preprocessor definition.
 *
 * Размеры соответствуют Windows на x86/x64 (LLP64): int = 4 байта,
 * short = 2 байта, char = 1 байт, double = 8 байт — совпадает с типами
 * из <stdint.h> в основной библиотеке, поэтому структуры бинарно идентичны.
 * -------------------------------------------------------------------------- */
#if defined(_MSC_VER)
    typedef unsigned __int8  zet017_uint8;
    typedef unsigned __int16 zet017_uint16;
    typedef unsigned __int32 zet017_uint32;
#else
    typedef unsigned char       zet017_uint8;
    typedef unsigned short      zet017_uint16;
    typedef unsigned int        zet017_uint32;
#endif

/* ZET017_CONFIG_API — соглашение о вызове для экспорта из DLL.
 * Совпадает с ZET017_TCP_API основной библиотеки (__stdcall на Windows),
 * чтобы LabView вызывал обе библиотеки единообразно.
 * В Call Library Function Node выберите Calling Convention = stdcall (WINAPI). */
#if defined(_WIN32)
#  define ZET017_CONFIG_API int __stdcall
#else
#  define ZET017_CONFIG_API int
#endif

/* --------------------------------------------------------------------------
 * ВАЖНО: структуры ниже бинарно идентичны struct zet017_config,
 * struct zet017_tenso_config и enum zet017_scheme из zet017tcp.h.
 * Они продублированы здесь намеренно — модуль zet017config полностью
 * независим от основной библиотеки и не требует её заголовка.
 *
 * Порядок и типы полей ДОЛЖНЫ совпадать с zet017tcp.h. Если структуры
 * в основной библиотеке изменятся, синхронизируйте их здесь.
 * -------------------------------------------------------------------------- */

enum zet017_cfg_scheme {
    ZET017_CFG_SCHEME_UNKNOWN        = -1,
    ZET017_CFG_SCHEME_BRIDGE         =  0,
    ZET017_CFG_SCHEME_HALF_BRIDGE,
    ZET017_CFG_SCHEME_QUARTER_BRIDGE,
};

struct zet017_cfg_config {
    zet017_uint32 sample_rate_adc;
    zet017_uint16 moda_adc;
    zet017_uint32 sample_rate_dac;
    zet017_uint16 rate_dac;
    zet017_uint32 mask_channel_adc;
    zet017_uint32 mask_icp;
    zet017_uint32 gain[8];
    zet017_uint16 gain_code[8];
    zet017_uint16 builtin_dac_state;
    double        builtin_dac_sine_freq;
    double        builtin_dac_sine_ampl;
    double        builtin_dac_sine_offset;
};

struct zet017_cfg_tenso_config {
    enum zet017_cfg_scheme scheme[8];
    zet017_uint8 correction_1[8];
    zet017_uint8 correction_2[8];
};

/* --------------------------------------------------------------------------
 * Единственная экспортируемая функция: разбор XML-файла в структуры.
 *
 * Ничего не отправляет на устройство — только читает файл и заполняет поля.
 * Отправку выполняет вызывающий код через основную библиотеку zet017tcp
 * (zet017_device_set_config / zet017_device_set_tenso_config).
 *
 * Рекомендуемая последовательность на стороне клиента:
 *   1) zet017_device_get_config(...)        — взять заводскую конфигурацию;
 *   2) zet017_device_get_tenso_config(...);
 *   3) zet017_config_load_file(...)         — наложить значения из XML;
 *   4) zet017_device_set_config(...)        — отправить на устройство;
 *   5) zet017_device_set_tenso_config(...).
 *
 * Параметры:
 *   file_name    — путь к XML-файлу конфигурации (UTF-8; рекомендуется
 *                  передавать АБСОЛЮТНЫЙ путь — относительный трактуется
 *                  относительно рабочего каталога процесса LabView);
 *   serial       — серийный номер устройства (ищется <Device serial="...">);
 *   config       — заполняемая структура конфигурации (обязательна);
 *   tenso_config — заполняемая структура тензо-конфигурации (может быть NULL,
 *                  тогда секция <Channels> пропускается).
 *
 * Приоритет значений (как в оригинале):
 *   ModaADC   обнуляет sample_rate_adc  (код режима имеет приоритет);
 *   RateDAC   обнуляет sample_rate_dac;
 *   KodAmplify обнуляет gain[i]         (сырой код усиления имеет приоритет).
 *
 * Возвращаемые значения:
 *    0  — успех;
 *   -1  — неверные аргументы (file_name или config == NULL);
 *   -2  — не удалось открыть или разобрать файл;
 *   -3  — корневой элемент не <Config>;
 *   -4  — отсутствует атрибут version;
 *   -5  — неподдерживаемая версия (ожидается 1.2);
 *   -6  — не найдено <Device> с указанным serial.
 * -------------------------------------------------------------------------- */
ZET017_CONFIG_API zet017_config_load_file(
    const char* file_name,
    zet017_uint32 serial,
    struct zet017_cfg_config* config,
    struct zet017_cfg_tenso_config* tenso_config);

/* --------------------------------------------------------------------------
 * Обратная операция: запись структур в XML-файл конфигурации.
 *
 * ВАЖНО: функция НЕ создаёт файл — она обновляет уже существующий.
 * Настоящий devices.cfg содержит около 30 элементов на устройство и 16 на
 * канал (name/type/configTime, Sense, units, Amplitude, DigitalResolChanADC,
 * Atten, DigitalInput/Output и т.д.). Их значения приходят от устройства в
 * служебной структуре, которую zet017tcp не объявляет в публичном API,
 * поэтому собрать корректный файл «с нуля» из zet017_cfg_config невозможно.
 * Функция обновляет только те элементы, которые сама читает, а остальное
 * содержимое файла переносит без изменений. Если файла нет — возвращается -2.
 *
 * Записываются:
 *   Channel, HCPChannel, ModaADC, Freq, RateDAC, KodAmplify (первые 8 позиций),
 *   BuiltinGenActive, BuiltinGenSineActive,
 *   BuiltinGenSineFreq, BuiltinGenSineAmpl, BuiltinGenSineBias,
 *   Channels/Channel[id=0..7]/Tenso, /Pot1, /Pot2 — только если tenso_config != NULL.
 *
 * Всё прочее — включая Rate, Amplitude, Atten, атрибуты каналов и канал
 * id=8 (генератор) — не изменяется.
 *
 * Приоритет значений совпадает с zet017_device_set_config: величина в
 * физических единицах имеет приоритет над сырым кодом, поэтому в файл
 * записывается именно то, что было бы передано устройству:
 *   sample_rate_adc != 0  →  ModaADC = код этой частоты, Freq = она сама;
 *   sample_rate_dac != 0  →  RateDAC = 80000000 / sample_rate_dac;
 *   gain[i]         != 0  →  KodAmplify[i] = код этого усиления.
 * Нулевое поле означает «не задано», тогда используется сырой код (moda_adc,
 * rate_dac, gain_code[i]).
 *
 * Параметры:
 *   dir_name     — каталог с файлом конфигурации (UTF-8; рекомендуется
 *                  АБСОЛЮТНЫЙ путь — относительный трактуется относительно
 *                  рабочего каталога процесса LabView);
 *   file_name    — имя файла в этом каталоге; NULL или "" означает
 *                  штатное имя "devices.cfg";
 *   serial       — серийный номер устройства (ищется <Device serial="...">);
 *   config       — записываемая структура конфигурации (обязательна);
 *   tenso_config — записываемая тензо-конфигурация (может быть NULL, тогда
 *                  секция <Channels> не изменяется).
 *
 * Файл заменяется целиком через временный файл рядом с ним, поэтому при
 * сбое записи исходный файл остаётся неизменным.
 *
 * Возвращаемые значения (та же диагностика — тот же номер, что в load):
 *    0  — успех;
 *   -1  — неверные аргументы (dir_name пустой/NULL или config == NULL);
 *   -2  — файл не найден или не разобран;
 *   -3  — корневой элемент не <Config>;
 *   -4  — отсутствует атрибут version;
 *   -5  — неподдерживаемая версия (ожидается 1.2);
 *   -6  — не найдено <Device> с указанным serial;
 *   -7  — не удалось записать файл (права доступа, нет места, сбой замены);
 *   -8  — слишком длинный путь (dir_name + file_name не помещаются в буфер).
 * -------------------------------------------------------------------------- */
ZET017_CONFIG_API zet017_config_save_file(
    const char* dir_name,
    const char* file_name,
    zet017_uint32 serial,
    const struct zet017_cfg_config* config,
    const struct zet017_cfg_tenso_config* tenso_config);

#ifdef __cplusplus
}
#endif

#endif /* ZET017_CONFIG_H */
