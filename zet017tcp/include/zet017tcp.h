#ifndef ZET017_TCP_H
#define ZET017_TCP_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Фиксированные целочисленные типы без подключения <stdint.h>.
 *
 * Мастер импорта LabView (Import Shared Library) часто не может разобрать
 * системный stdint.h из современного MSVC (он не самодостаточен и тянет
 * vcruntime.h/внутренние заголовки), из-за чего uint32_t и др. остаются
 * неопределёнными. Поэтому объявляем нужные типы вручную — заголовок
 * становится самодостаточным, и в мастере НЕ нужно указывать ни одного
 * пути к заголовкам и ни одной preprocessor definition.
 *
 * Размеры соответствуют Windows на x86/x64: char=1, short=2, int=4,
 * long long=8, double=8 — совпадает с типами <stdint.h>, поэтому
 * ABI структур не меняется. */
#if defined(_MSC_VER)
    typedef signed   __int32 zet017_int32;
    typedef unsigned __int8  zet017_uint8;
    typedef unsigned __int16 zet017_uint16;
    typedef unsigned __int32 zet017_uint32;
    typedef unsigned __int64 zet017_uint64;
#else
    typedef signed   int       zet017_int32;
    typedef unsigned char      zet017_uint8;
    typedef unsigned short     zet017_uint16;
    typedef unsigned int       zet017_uint32;
    typedef unsigned long long zet017_uint64;
#endif

/* ZET017_TCP_API — соглашение о вызове для экспорта из DLL.
 * Намеренно НЕ включаем <windows.h> в публичный заголовок,
 * чтобы избежать конфликтов имён с макросами WinAPI.
 * WINAPI = __stdcall — определяем напрямую. */
#if defined(_WIN32)
#  define ZET017_TCP_API int __stdcall
#else
#  define ZET017_TCP_API int
#endif

/* Непрозрачный дескриптор сервера.
 * LabView работает только с этим значением (int32_t).
 * Структура zet017_server скрыта внутри DLL.
 * 0 означает недействительный дескриптор. */
typedef zet017_int32 zet017_handle;

enum zet017_scheme {
    unknown        = -1,
    bridge         =  0,
    half_bridge,
    quarter_bridge,
};

struct zet017_config {
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

struct zet017_tenso_config {
    enum zet017_scheme scheme[8];
    zet017_uint8 correction_1[8];
    zet017_uint8 correction_2[8];
};

struct zet017_info {
    char          ip[16];
    char          name[16];
    zet017_uint32 serial;
    char          version[32];
};

struct zet017_state {
    zet017_uint16 is_connected;
    zet017_uint16 reserved;     /* явный паддинг вместо неявного выравнивания */
    zet017_uint32 reconnect;    /* было zet017_uint64 — несовместимо с выравниванием LabView x86 */
    zet017_uint32 pointer_adc;
    zet017_uint32 buffer_size_adc;
    zet017_uint32 pointer_dac;
    zet017_uint32 buffer_size_dac;
};

/* --------------------------------------------------------------------------
 * Логирование.
 *
 * Уровень важности задаётся отдельно для каждой категории сообщений.
 * Каждый уровень включает все предыдущие: info пишет error + warning + info.
 *
 * В LabView параметры level и category — обычные I32 (мастер импорта не
 * переносит имена констант, числовые значения см. в README). */
enum zet017_log_level {
    zet017_log_off     = 0,  /* лог выключен полностью                        */
    zet017_log_error   = 1,  /* отказ ресурса или подсистемы                  */
    zet017_log_warning = 2,  /* ожидаемая отработанная ситуация               */
    zet017_log_info    = 3   /* штатные события                              */
};

enum zet017_log_category {
    zet017_cat_api     = 0,  /* вход/выход публичных функций, валидация       */
    zet017_cat_network = 1,  /* подключения, обрывы, reconnect, handshake     */
    zet017_cat_command = 2,  /* выполнение команд устройства                  */
    zet017_cat_data    = 3,  /* поток АЦП/ЦАП: приём пакетов, переполнение    */
    zet017_cat_count   = 4
};

/* Создать сервер. Возвращает 0 при успехе, отрицательное при ошибке.
 * handle заполняется дескриптором > 0.
 * log_path — путь к отладочному лог-файлу (например "C:\\zet017_debug.txt").
 * Путь должен быть АБСОЛЮТНЫМ: текущая директория у LabView непредсказуема.
 * Передайте NULL или "" чтобы отключить логирование.
 * Эквивалент zet017_server_create_ex(handle, log_path, zet017_log_info).
 *
 * Если файл открыть не удалось, сервер создаётся нормально, а лог молча
 * отключается — логирование не должно ломать инициализацию. Чтобы получить
 * код ошибки открытия, используйте create(handle, NULL) с последующим
 * zet017_server_set_log_file(). */
ZET017_TCP_API zet017_server_create(zet017_handle* handle, const char* log_path);

/* Создать сервер с заданным уровнем лога (применяется ко всем категориям).
 * Коды возврата: 0 успех, -1 handle==NULL, -2 network_init,
 * -3 malloc, -4 mutex_init. */
ZET017_TCP_API zet017_server_create_ex(
    zet017_handle* handle, const char* log_path, zet017_int32 level);

/* Установить уровень лога сразу для всех категорий. Потокобезопасно.
 * Коды возврата: 0 успех, -1 невалидный handle, -3 невалидный level. */
ZET017_TCP_API zet017_server_set_log_level(
    zet017_handle handle, zet017_int32 level);

/* Переопределить уровень для одной категории. Потокобезопасно.
 * Коды возврата: 0 успех, -1 невалидный handle,
 * -2 невалидная category, -3 невалидный level. */
ZET017_TCP_API zet017_server_set_log_category_level(
    zet017_handle handle, zet017_int32 category, zet017_int32 level);

/* Сменить лог-файл на лету. Новый путь открывается до закрытия старого,
 * поэтому при неудаче прежнее состояние сохраняется. NULL или "" —
 * выключить файловый лог (и отпустить файл). Потокобезопасно.
 * Коды возврата: 0 успех, -1 невалидный handle,
 * -4 не удалось открыть файл или путь длиннее 259 символов. */
ZET017_TCP_API zet017_server_set_log_file(
    zet017_handle handle, const char* log_path);

/* Освободить сервер и все его устройства. Обнуляет *handle. */
ZET017_TCP_API zet017_server_free(zet017_handle* handle);

/* Добавить устройство по IP-адресу. */
ZET017_TCP_API zet017_server_add_device(zet017_handle handle, const char* ip);

/* Удалить устройство по IP-адресу. */
ZET017_TCP_API zet017_server_remove_device(zet017_handle handle, const char* ip);

/* Получить информацию об устройстве (имя, серийный номер, версия). */
ZET017_TCP_API zet017_device_get_info(zet017_handle handle, zet017_uint32 number, struct zet017_info* info);

/* Получить состояние устройства (подключение, указатели буферов). */
ZET017_TCP_API zet017_device_get_state(zet017_handle handle, zet017_uint32 number, struct zet017_state* state);

/* Получить текущую конфигурацию устройства. */
ZET017_TCP_API zet017_device_get_config(zet017_handle handle, zet017_uint32 number, struct zet017_config* config);

/* Получить конфигурацию тензодатчиков. */
ZET017_TCP_API zet017_device_get_tenso_config(zet017_handle handle, zet017_uint32 number, struct zet017_tenso_config* config);

/* Установить конфигурацию устройства. */
ZET017_TCP_API zet017_device_set_config(zet017_handle handle, zet017_uint32 number, const struct zet017_config* config);

/* Установить конфигурацию тензодатчиков. */
ZET017_TCP_API zet017_device_set_tenso_config(zet017_handle handle, zet017_uint32 number, const struct zet017_tenso_config* config);

/* Запустить АЦП (и опционально ЦАП если dac != 0). */
ZET017_TCP_API zet017_device_start(zet017_handle handle, zet017_uint32 number, zet017_uint32 dac);

/* Остановить АЦП/ЦАП. */
ZET017_TCP_API zet017_device_stop(zet017_handle handle, zet017_uint32 number);

/* Прочитать данные канала АЦП (в вольтах). */
ZET017_TCP_API zet017_channel_get_data(
    zet017_handle handle, zet017_uint32 number, zet017_uint32 channel,
    zet017_uint32 pointer, float* data, zet017_uint32 size);

/* --------------------------------------------------------------------------
 * Прочитать все активные каналы АЦП за один вызов (в вольтах).
 *
 * Отличие от zet017_channel_get_data не только в удобстве: все каналы
 * снимаются под ОДНИМ захватом внутреннего мьютекса, то есть из одного и
 * того же окна. При поканальном чтении мьютекс отпускается между вызовами,
 * и поток приёма успевает затереть начало окна — тогда первый канал
 * прочитан до перезаписи, а последний уже после. Для расчётов, где каналы
 * сравниваются между собой (измерительный и опорный), это критично.
 *
 * Единицы pointer и size — те же, что у zet017_channel_get_data: отсчёты
 * НА КАНАЛ, согласованные с zet017_state.pointer_adc и buffer_size_adc.
 * Чтение идёт НАЗАД: возвращается окно [pointer - size, pointer).
 *
 * Две функции различаются ТОЛЬКО раскладкой приёмника (n — число активных
 * каналов, оно же возвращается в channels):
 *
 *   zet017_channel_get_all_data        data[k * size + i]
 *     Отсчёты каждого канала лежат подряд. В LabView объявляется как
 *     2D Array of Single: строки — каналы, столбцы — отсчёты.
 *
 *   zet017_channel_get_all_frame_data  data[i * n + k]
 *     Каналы чередуются внутри кадра, как в самом кольце. В LabView это
 *     плоский 1D Array of Single, раскладку делает вызывающий.
 *
 * ВАЖНО: k — порядковый номер среди ВКЛЮЧЁННЫХ каналов, а не номер канала.
 * При маске 0b101 (каналы 0 и 2) k=0 это канал 0, а k=1 — канал 2.
 * Соответствие восстанавливается из zet017_config.mask_channel_adc.
 *
 * Параметры:
 *   pointer   — позиция, ОТ которой читаем назад (отсчётов на канал);
 *   data      — приёмник, выделяет вызывающий; не может быть NULL;
 *   size      — сколько отсчётов НА КАНАЛ вернуть;
 *   capacity  — сколько элементов float реально вмещает data. Библиотека
 *               не может узнать размер чужого буфера, поэтому без этого
 *               параметра ошибка вызывающего означала бы запись за его
 *               границы. Требуется capacity >= channels * size;
 *   channels  — сюда записывается число активных каналов. Можно передать
 *               NULL. Заполняется и при отказе -7 тоже, чтобы вызывающий
 *               узнал, какой буфер нужен, не угадывая.
 *
 * Возвращаемые значения (нумерация та же, что у zet017_channel_get_data):
 *    0  — успех;
 *   -1  — невалидный handle или устройство не найдено;
 *   -3  — нет связи с устройством;
 *   -4  — data == NULL;
 *   -6  — pointer или size выходят за размер кольцевого буфера;
 *   -7  — capacity мала; в *channels записано требуемое число каналов,
 *         в data не записано ничего.
 * -------------------------------------------------------------------------- */
ZET017_TCP_API zet017_channel_get_all_data(
    zet017_handle handle, zet017_uint32 number, zet017_uint32 pointer,
    float* data, zet017_uint32 size, zet017_uint32 capacity,
    zet017_uint32* channels);

ZET017_TCP_API zet017_channel_get_all_frame_data(
    zet017_handle handle, zet017_uint32 number, zet017_uint32 pointer,
    float* data, zet017_uint32 size, zet017_uint32 capacity,
    zet017_uint32* channels);

/* Записать данные канала ЦАП (в вольтах). */
ZET017_TCP_API zet017_channel_put_data(
    zet017_handle handle, zet017_uint32 number, zet017_uint32 channel,
    zet017_uint32 pointer, float* data, zet017_uint32 size);

#ifdef __cplusplus
}
#endif

#endif /* ZET017_TCP_H */
