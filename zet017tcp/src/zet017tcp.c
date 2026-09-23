#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define ZET017_TCP_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#define socket_t SOCKET
#define close_socket(s) closesocket(s)
typedef HANDLE            thread_t;
typedef CRITICAL_SECTION  mutex_t;
typedef CONDITION_VARIABLE cond_t;
#define THREAD_RETURN DWORD WINAPI
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define socket_t int
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
#define close_socket(s) close(s)
typedef pthread_t       thread_t;
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t  cond_t;
#define THREAD_RETURN void*
#endif

#include "zet017tcp.h"

/* ------------------------------------------------------------------ */
/*  Логирование: объявления                                             */
/* ------------------------------------------------------------------ */

/* Максимальная длина пути к лог-файлу (включая '\0') */
#define ZET017_LOG_PATH_MAX 260

/* Максимальная длина одного сообщения после форматирования.
 * Избыток молча обрезается. */
#define ZET017_LOG_MSG_MAX 1024

/* Размер буфера потока — см. политику сброса ниже */
#define ZET017_LOG_BUF_SIZE (32 * 1024)

/* Строки уровня info сбрасываются на диск не чаще одного раза в столько
 * миллисекунд; error и warning — немедленно.
 *
 * Причина: zet017_channel_get_data вызывается из цикла сбора LabView, и
 * fflush на каждой строке в режиме CLFN "Run in UI Thread" блокирует
 * лицевую панель, а в любом режиме сериализует все устройства на одном
 * мьютексе логгера. */
#define ZET017_LOG_FLUSH_INTERVAL_MS 250

/* Версия для баннера сессии; обычно приходит из CMake */
#ifndef ZET017TCP_VERSION
#define ZET017TCP_VERSION "0.0.0"
#endif

struct zet017_logger {
    int      level[zet017_cat_count]; /* порог по каждой категории       */
    char     path[ZET017_LOG_PATH_MAX];
    FILE*    file;                    /* открыт один раз, NULL = выкл.   */
    mutex_t  mutex;
    int      ready;                   /* мьютекс создан                  */
    uint32_t last_flush;              /* метка последнего fflush, мс     */
};

struct zet017_server;

static void zet017_log_write(struct zet017_server* server, int category, int level,
                             const char* fmt, ...);

/* ------------------------------------------------------------------ */
/*  Константы                                                           */
/* ------------------------------------------------------------------ */

#define MAX_IP_LENGTH 16

#define ZET017_CMD_PORT 1808
#define ZET017_ADC_PORT 2320
#define ZET017_DAC_PORT 3344

#define ZET017_CMD_GET_INFO        0x0000
#define ZET017_CMD_PUT_INFO        0x0012
#define ZET017_CMD_READ_CORRECTION 0x0513
#define ZET017_CMD_WRITE_TENSO     0x0570
#define ZET017_CMD_READ_TENSO      0x0571

#define ZET017_PACKET_SIZE    1024
#define ZET017_MAX_FLUSH_SIZE 2048

#define ZET017_MAX_SAMPLE_RATE_ADC  50000
#define ZET017_MAX_CHANNELS_ADC     8
#define ZET017_MAX_GAINS_ADC        4
#define ZET017_MAX_SAMPLE_SIZE_ADC  sizeof(int32_t)
#define ZET017_MAX_ADC_BUFFER_SIZE  (ZET017_MAX_SAMPLE_RATE_ADC * (ZET017_MAX_CHANNELS_ADC + 1) * ZET017_MAX_SAMPLE_SIZE_ADC) * 2
#define ZET017_ADC_GR_BUFFER_SIZE   (1 * 2 * 3 * 2 * 5 * 1 * 7 * 2 * 3 * sizeof(int32_t))
#define ZET017_ADC_BUFFER_SIZE      (ZET017_MAX_ADC_BUFFER_SIZE / ZET017_ADC_GR_BUFFER_SIZE + 1) * ZET017_ADC_GR_BUFFER_SIZE

#define ZET017_MAX_SAMPLE_RATE_DAC  200000
#define ZET017_MAX_CHANNELS_DAC     2
#define ZET017_MAX_SAMPLE_SIZE_DAC  sizeof(int32_t)
#define ZET017_MAX_DAC_BUFFER_SIZE  (ZET017_MAX_SAMPLE_RATE_DAC * ZET017_MAX_CHANNELS_DAC * ZET017_MAX_SAMPLE_SIZE_DAC)
#define ZET017_DAC_BUFFER_SIZE      ZET017_MAX_DAC_BUFFER_SIZE * 4

/* ------------------------------------------------------------------ */
/*  Внутренние перечисления                                             */
/* ------------------------------------------------------------------ */

enum zet017_command {
    zet017_set_config = 0,
    zet017_write_tenso_config,
    zet017_start,
    zet017_stop,
};

enum zet017_command_state {
    zet017_command_idle = 0,
    zet017_command_requested,
    zet017_command_processing,
    zet017_command_completed,
};

/* ------------------------------------------------------------------ */
/*  Внутренние структуры (скрыты от LabView)                            */
/* ------------------------------------------------------------------ */

struct zet017_device_info {
    uint16_t command;
    uint8_t  reserve_1[2];
    int16_t  start_adc;
    int16_t  start_dac;
    uint8_t  reserve_2[6];
    uint16_t quantity_channel_adc;
    uint16_t quantity_channel_dac;
    uint8_t  type_data_adc;
    uint8_t  type_data_dac;
    uint32_t mask_channel_adc;
    uint32_t mask_channel_dac;
    uint32_t mask_icp;
    uint8_t  reserve_3[4];
    uint16_t work_channel_adc;
    uint16_t work_channel_dac;
    uint16_t amplify_code[8];
    uint8_t  reserve_4[112];
    uint16_t atten[4];
    uint8_t  reserve_5[10];
    uint16_t mode_adc;
    uint8_t  reserve_6[2];
    uint16_t rate_dac;
    uint16_t size_packet_adc;
    uint8_t  reserve_7[22];
    uint32_t digital_input;
    uint32_t digital_output;
    uint8_t  reserve_8[12];
    char     version_dsp[32];
    char     device_name[16];
    uint8_t  reserve_9[16];
    uint32_t serial;
    uint8_t  reserve_10[12];
    uint32_t digital_output_enable;
    float    resolution_adc_def;
    uint8_t  reserve_11[4];
    float    resolution_dac_def;
    uint8_t  reserve_12[4];
    float    resolution_adc[16];
    uint8_t  reserve_13[10];
    uint16_t builtin_dac_state;
    int32_t  builtin_dac_sine_freq;
    int32_t  builtin_dac_sine_ampl;
    int32_t  builtin_dac_sine_offset;
    uint8_t  reserve_14[14];
    uint16_t atten_speed;
    uint8_t  reserve_15[24];
    float    resolution_dac[4];
    uint8_t  reserve_16[8];
    uint16_t quantity_channel_virt;
    uint8_t  reserve_17[22];
};

struct zet017_correction_info {
    float amplify[ZET017_MAX_CHANNELS_ADC][ZET017_MAX_GAINS_ADC];
    float offset_adc[ZET017_MAX_CHANNELS_ADC][ZET017_MAX_GAINS_ADC];
    float reduction[ZET017_MAX_CHANNELS_DAC];
    float offset_dac[ZET017_MAX_CHANNELS_DAC];
};

struct zet017_tenso_info {
    uint16_t scheme[ZET017_MAX_CHANNELS_ADC];
    uint8_t  correction[ZET017_MAX_CHANNELS_ADC][2];
};

struct zet017_command_info {
    uint16_t command;
    uint16_t error;
    uint32_t size;
    union {
        uint16_t u16[(1024 - 8) / 2];
        uint8_t  u8[1024 - 8];
    } data;
};

union zet017_packet {
    uint8_t                  raw[ZET017_PACKET_SIZE];
    struct zet017_device_info info;
    struct zet017_command_info cmd;
};

struct zet017_command_data {
    union zet017_packet      data;
    enum zet017_command      command;
    enum zet017_command_state state;
    int                      result;
    mutex_t                  mutex;
    cond_t                   cond;
};

struct zet017_adc_data {
    uint8_t  buffer[ZET017_ADC_BUFFER_SIZE];
    uint32_t pointer;
    uint64_t frames_total;  /* кадров записано с начала сессии (детект перезаписи) */
    uint32_t channel_mask;
    uint16_t work_channel;
    uint16_t channel_quantity;
    uint16_t sample_size;
    uint16_t amplify_code[ZET017_MAX_CHANNELS_ADC + 1];
    float    resolution[ZET017_MAX_CHANNELS_ADC + 1][ZET017_MAX_GAINS_ADC];
    mutex_t  mutex;
};

struct zet017_dac_data {
    uint8_t  buffer[ZET017_DAC_BUFFER_SIZE];
    uint32_t pointer;
    uint32_t channel_mask;
    uint16_t channel_quantity;
    uint16_t sample_size;
    float    resolution[ZET017_MAX_CHANNELS_DAC];
    mutex_t  mutex;
};

struct zet017_adc_dac_data {
    uint32_t sample_rate_adc;
    uint16_t sample_size_adc;
    uint16_t work_channel_adc;
    uint64_t adc_count;
    uint32_t sample_rate_dac;
    uint16_t work_channel_dac;
    uint16_t sample_size_dac;
    uint64_t dac_count;
};

struct zet017_device {
    char     ip[MAX_IP_LENGTH];
    socket_t cmd_socket;
    socket_t adc_socket;
    socket_t dac_socket;
    socket_t wakeup_socket[2];

    thread_t work_thread;
    uint16_t running;

    uint16_t is_connected;
    uint32_t reconnect;   /* было uint64_t — синхронно с публичным zet017_state.reconnect */
    uint32_t timestamp;
    struct zet017_device_info  device_info;
    struct zet017_tenso_info   tenso_info;
    struct zet017_adc_dac_data adc_dac_data;

    struct zet017_state state;
    mutex_t             state_mutex;

    struct zet017_info info;
    mutex_t            info_mutex;

    struct zet017_config       config;
    struct zet017_tenso_config tenso_config;
    mutex_t                    config_mutex;

    struct zet017_command_data command;

    struct zet017_adc_data adc_data;
    struct zet017_dac_data dac_data;

    struct zet017_correction_info correction;

    /* Сервер-владелец: рабочий поток получает отсюда логгер.
     * Заполняется в zet017_server_add_device. */
    struct zet017_server* owner;

    /* Метки времени для ограничения частоты сообщений категории data */
    uint32_t log_overrun_ts;
    uint32_t log_rate_ts;

    struct zet017_device* next;
};

/* Полное определение скрыто от LabView — в .h только forward declaration убран,
 * typedef zet017_handle = int32_t */
struct zet017_server {
    struct zet017_device* devices;
    size_t                device_count;
    mutex_t               devices_mutex;
    struct zet017_logger  logger;
};

/* ------------------------------------------------------------------ */
/*  Примитивы синхронизации                                             */
/* ------------------------------------------------------------------ */

static int zet017_mutex_init(mutex_t* mutex) {
#if defined(ZET017_TCP_WINDOWS)
    InitializeCriticalSection(mutex);
#else
    if (pthread_mutex_init(mutex, NULL) != 0)
        return -1;
#endif
    return 0;
}

static void zet017_mutex_destroy(mutex_t* mutex) {
#if defined(ZET017_TCP_WINDOWS)
    DeleteCriticalSection(mutex);
#else
    pthread_mutex_destroy(mutex);
#endif
}

static void zet017_mutex_lock(mutex_t* mutex) {
#if defined(ZET017_TCP_WINDOWS)
    EnterCriticalSection(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}

static void zet017_mutex_unlock(mutex_t* mutex) {
#if defined(ZET017_TCP_WINDOWS)
    LeaveCriticalSection(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}

static int zet017_cond_init(cond_t* cond) {
#if defined(ZET017_TCP_WINDOWS)
    InitializeConditionVariable(cond);
#else
    if (pthread_cond_init(cond, NULL) != 0)
        return -1;
#endif
    return 0;
}

static void zet017_cond_destroy(cond_t* cond) {
#if !defined(ZET017_TCP_WINDOWS)
    pthread_cond_destroy(cond);
#endif
}

static void zet017_cond_wait(cond_t* cond, mutex_t* mutex) {
#if defined(ZET017_TCP_WINDOWS)
    SleepConditionVariableCS(cond, mutex, INFINITE);
#else
    pthread_cond_wait(cond, mutex);
#endif
}

static void zet017_cond_signal(cond_t* cond) {
#if defined(ZET017_TCP_WINDOWS)
    WakeConditionVariable(cond);
#else
    pthread_cond_signal(cond);
#endif
}

/* ------------------------------------------------------------------ */
/*  Реестр дескрипторов (opaque handle → zet017_server*)               */
/* ------------------------------------------------------------------ */

#define ZET017_MAX_SERVERS 16

static struct zet017_server* g_servers[ZET017_MAX_SERVERS];
static mutex_t               g_registry_mutex;
static int                   g_registry_initialized = 0;

static void registry_init(void) {
    if (g_registry_initialized)
        return;
    zet017_mutex_init(&g_registry_mutex);
    memset(g_servers, 0, sizeof(g_servers));
    g_registry_initialized = 1;
}

/* Возвращает handle 1..ZET017_MAX_SERVERS либо 0, если свободных слотов нет. */
static zet017_handle registry_add(struct zet017_server* server) {
    zet017_mutex_lock(&g_registry_mutex);
    for (int i = 0; i < ZET017_MAX_SERVERS; ++i) {
        if (g_servers[i] == NULL) {
            g_servers[i] = server;
            zet017_mutex_unlock(&g_registry_mutex);
            return (zet017_handle)(i + 1);
        }
    }
    zet017_mutex_unlock(&g_registry_mutex);
    return 0;
}

/* Получить указатель по handle. Не требует блокировки —
 * запись в слот происходит только в registry_add/registry_remove,
 * которые не вызываются конкурентно с теми же handle. */
static struct zet017_server* registry_get(zet017_handle handle) {
    if (handle < 1 || handle > ZET017_MAX_SERVERS)
        return NULL;
    return g_servers[handle - 1];
}

static void registry_remove(zet017_handle handle) {
    if (handle < 1 || handle > ZET017_MAX_SERVERS)
        return;
    zet017_mutex_lock(&g_registry_mutex);
    g_servers[handle - 1] = NULL;
    zet017_mutex_unlock(&g_registry_mutex);
}

/* ------------------------------------------------------------------ */
/*  Сеть                                                                */
/* ------------------------------------------------------------------ */

static int network_init(void) {
#if defined(ZET017_TCP_WINDOWS)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
        return -1;
#endif
    return 0;
}

static void network_cleanup(void) {
#if defined(ZET017_TCP_WINDOWS)
    WSACleanup();
#endif
}

/* ------------------------------------------------------------------ */
/*  Вспомогательные функции                                             */
/* ------------------------------------------------------------------ */

static uint32_t zet017_get_timestamp(void) {
#if defined(ZET017_TCP_WINDOWS)
    /* GetTickCount64 не переполняется каждые 49 дней в отличие от GetTickCount.
     * Используются младшие 32 бита — для вычисления разницы timestamp'ов
     * (timestamp - device->timestamp > 60000) это безопасно: разница
     * двух uint32_t корректно обрабатывает переполнение при вычитании. */
    return (uint32_t)(GetTickCount64() & 0xFFFFFFFF);
#else
    struct timespec ts;
    uint32_t ms;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        ms  = (uint32_t)(ts.tv_sec * 1000);
        ms += (uint32_t)(ts.tv_nsec + 500000) / 1000000;
    } else {
        ms = 0;
    }
    return ms;
#endif
}

/* ------------------------------------------------------------------ */
/*  Логирование: реализация                                             */
/* ------------------------------------------------------------------ */

static int zet017_last_error(void) {
#if defined(ZET017_TCP_WINDOWS)
    return (int)WSAGetLastError();
#else
    return errno;
#endif
}

static const char* zet017_log_level_tag(int level) {
    switch (level) {
    case zet017_log_error:   return "ERROR";
    case zet017_log_warning: return "WARN";
    default:                 return "INFO";
    }
}

static const char* zet017_log_level_name(int level) {
    switch (level) {
    case zet017_log_off:     return "off";
    case zet017_log_error:   return "error";
    case zet017_log_warning: return "warning";
    default:                 return "info";
    }
}

static const char* zet017_log_cat_tag(int category) {
    switch (category) {
    case zet017_cat_network: return "NET";
    case zet017_cat_command: return "CMD";
    case zet017_cat_data:    return "DATA";
    default:                 return "API";
    }
}

/* Метка времени "YYYY-MM-DD HH:MM:SS.mmm" — локальное время машины.
 * Дата обязательна: приборы работают сутками. */
static void zet017_log_time(char* buf, size_t size) {
#if defined(ZET017_TCP_WINDOWS)
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    struct tm       tm_buf;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm_buf);
    snprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(ts.tv_nsec / 1000000));
#endif
}

static unsigned long zet017_log_pid(void) {
#if defined(ZET017_TCP_WINDOWS)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

/* Имя процесса-хоста без пути. В логе нужно, чтобы различать среду
 * разработки LabView и собранный EXE, пишущие в один файл. */
static void zet017_log_exe_name(char* buf, size_t size) {
    buf[0] = '\0';
#if defined(ZET017_TCP_WINDOWS)
    {
        char  path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
        if (n > 0 && n < (DWORD)sizeof(path)) {
            const char* base = strrchr(path, '\\');
            snprintf(buf, size, "%s", base ? base + 1 : path);
        }
    }
#else
    {
        char    path[512];
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        if (n > 0) {
            path[n] = '\0';
            const char* base = strrchr(path, '/');
            snprintf(buf, size, "%s", base ? base + 1 : path);
        }
    }
#endif
    if (buf[0] == '\0')
        snprintf(buf, size, "unknown");
}

/* Баннер сессии. Вызывается с ЗАХВАЧЕННЫМ мьютексом логгера. */
static void zet017_log_banner_locked(struct zet017_logger* logger) {
    char stamp[32];
    char exe[64];

    if (!logger->file)
        return;

    zet017_log_time(stamp, sizeof(stamp));
    zet017_log_exe_name(exe, sizeof(exe));

    fprintf(logger->file,
            "=== %s zet017tcp %s (%s) log session started (level=%s) pid=%lu exe=%s ===\n",
            stamp, ZET017TCP_VERSION,
            sizeof(void*) == 8 ? "x64" : "x86",
            zet017_log_level_name(logger->level[zet017_cat_api]),
            zet017_log_pid(), exe);
    fflush(logger->file);
    logger->last_flush = zet017_get_timestamp();
}

/* Открыть/сменить/закрыть файл. Вызывается с ЗАХВАЧЕННЫМ мьютексом.
 * Возвращает 0 при успехе, -1 если файл открыть не удалось (прежнее
 * состояние при этом сохраняется). */
static int zet017_logger_open_locked(struct zet017_logger* logger, const char* path) {
    FILE* file;

    if (!path || path[0] == '\0') {
        if (logger->file) {
            fflush(logger->file);
            fclose(logger->file);
        }
        logger->file    = NULL;
        logger->path[0] = '\0';
        return 0;
    }

    if (strlen(path) >= ZET017_LOG_PATH_MAX)
        return -1;

    file = fopen(path, "a");
    if (!file)
        return -1;

    /* Буферизация: строки info не должны вызывать обращение к диску на
     * каждой итерации цикла опроса LabView (см. ZET017_LOG_FLUSH_INTERVAL_MS). */
    setvbuf(file, NULL, _IOFBF, ZET017_LOG_BUF_SIZE);

    /* Старый файл закрываем только после успешного открытия нового */
    if (logger->file) {
        fflush(logger->file);
        fclose(logger->file);
    }
    logger->file = file;
    strncpy(logger->path, path, ZET017_LOG_PATH_MAX - 1);
    logger->path[ZET017_LOG_PATH_MAX - 1] = '\0';

    zet017_log_banner_locked(logger);
    return 0;
}

static int zet017_logger_init(struct zet017_logger* logger, const char* path, int level) {
    memset(logger, 0, sizeof(*logger));

    if (level < zet017_log_off)  level = zet017_log_off;
    if (level > zet017_log_info) level = zet017_log_info;
    for (int i = 0; i < zet017_cat_count; ++i)
        logger->level[i] = level;

    if (zet017_mutex_init(&logger->mutex) != 0)
        return -1;
    logger->ready      = 1;
    logger->last_flush = zet017_get_timestamp();

    zet017_mutex_lock(&logger->mutex);
    /* Неудача открытия не фатальна: лог молча отключается, сервер работает */
    (void)zet017_logger_open_locked(logger, path);
    zet017_mutex_unlock(&logger->mutex);
    return 0;
}

static void zet017_logger_destroy(struct zet017_logger* logger) {
    if (!logger->ready)
        return;
    zet017_mutex_lock(&logger->mutex);
    if (logger->file) {
        fflush(logger->file);
        fclose(logger->file);
        logger->file = NULL;
    }
    zet017_mutex_unlock(&logger->mutex);
    zet017_mutex_destroy(&logger->mutex);
    logger->ready = 0;
}

static void zet017_log_write(struct zet017_server* server, int category, int level,
                             const char* fmt, ...)
{
    struct zet017_logger* logger;
    char    msg[ZET017_LOG_MSG_MAX];
    char    stamp[32];
    va_list ap;

    if (!server)
        return;
    logger = &server->logger;
    if (!logger->ready || !logger->file)
        return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    zet017_log_time(stamp, sizeof(stamp));

    zet017_mutex_lock(&logger->mutex);
    if (logger->file) {
        fprintf(logger->file, "[%s] [%s] [%s] %s\n",
                stamp, zet017_log_level_tag(level), zet017_log_cat_tag(category), msg);

        if (level <= zet017_log_warning) {
            fflush(logger->file);
            logger->last_flush = zet017_get_timestamp();
        } else {
            uint32_t now = zet017_get_timestamp();
            if (now - logger->last_flush >= ZET017_LOG_FLUSH_INTERVAL_MS) {
                fflush(logger->file);
                logger->last_flush = now;
            }
        }
    }
    zet017_mutex_unlock(&logger->mutex);
}

/* Лог до того, как сервер существует (внутри zet017_server_create_ex).
 * Открывает и закрывает файл на каждое сообщение — вызывается считанные
 * разы за время работы процесса. */
static void zet017_log_early(const char* path, int enabled_level, int level, const char* msg) {
    char  stamp[32];
    FILE* f;

    if (!path || path[0] == '\0' || level > enabled_level)
        return;
    f = fopen(path, "a");
    if (!f)
        return;
    zet017_log_time(stamp, sizeof(stamp));
    fprintf(f, "[%s] [%s] [API] %s\n", stamp, zet017_log_level_tag(level), msg);
    fclose(f);
}

/* Проверка уровня выполняется ДО форматирования и ДО захвата мьютекса:
 * затраты на отфильтрованное сообщение — одно сравнение. Гонка чтения
 * уровня с set_log_level безвредна.
 *
 * ВАЖНО: SLOG нельзя вызывать, удерживая любой мьютекс устройства или
 * сервера. Логгер захватывает собственный мьютекс, и порядок захвата
 * оказался бы обратным тому, которого придерживаются рабочие потоки, что
 * приводит к взаимной блокировке. Факты собираются в локальные переменные под
 * мьютексом, а запись в лог идёт после его освобождения. */
#define SLOG(s, cat, lvl, ...)                             \
    do {                                                   \
        struct zet017_server* zet017_slog_s = (s);          \
        if (zet017_slog_s && (lvl) <= zet017_slog_s->logger.level[cat]) \
            zet017_log_write(zet017_slog_s, (cat), (lvl), __VA_ARGS__); \
    } while (0)

/* Лог из контекста устройства: сервер берётся из device->owner */
#define DLOG(d, cat, lvl, ...) SLOG((d) ? (d)->owner : NULL, cat, lvl, __VA_ARGS__)

static uint32_t zet017_get_sample_rate_adc(uint16_t mode_adc) {
    switch (mode_adc) {
    case 1: return 50000;
    case 3: return 5000;
    case 4: return 2500;
    default: break;
    }
    return 25000;
}

static uint16_t zet017_get_mode_adc(uint32_t sample_rate_adc) {
    switch (sample_rate_adc) {
    case 50000: return 1;
    case 25000: return 2;
    case 5000:  return 3;
    case 2500:  return 4;
    default:    break;
    }
    return 0;
}

static uint32_t zet017_get_sample_rate_dac(uint16_t rate_dac) {
    return rate_dac ? 80000000 / rate_dac : 0;
}

static uint16_t zet017_get_rate_dac(uint32_t sample_rate_dac) {
    return sample_rate_dac ? 80000000 / sample_rate_dac : 0;
}

static uint32_t zet017_get_gain(uint16_t amplify_code) {
    switch (amplify_code) {
    case 0: return 1;
    case 1: return 10;
    case 2: return 100;
    default: break;
    }
    return 0;
}

static uint32_t zet017_get_amplify_code(uint32_t gain) {
    switch (gain) {
    case 1:   return 0;
    case 10:  return 1;
    case 100: return 2;
    default:  break;
    }
    return 0;
}

static void zet017_set_size_packet_adc(struct zet017_device_info* info) {
    uint16_t work_channel_adc = 0;
    for (uint16_t i = 0; i < info->quantity_channel_adc; ++i) {
        uint16_t j = 1 << (info->quantity_channel_adc == 4 ? i * 2 + 1 : i);
        if (info->mask_channel_adc & j)
            ++work_channel_adc;
    }
    uint32_t sample_size      = (uint32_t)(info->type_data_adc == 0 ? sizeof(int16_t) : sizeof(int32_t));
    uint32_t max_bytes_count  = ZET017_PACKET_SIZE - sizeof(uint64_t);
    uint32_t max_samples_count = max_bytes_count / sample_size;
    uint32_t max_frames_count  = max_samples_count / work_channel_adc;
    uint32_t sample_rate_adc   = zet017_get_sample_rate_adc(info->mode_adc);

    for (;;) {
        uint32_t count = sample_rate_adc / max_frames_count;
        if (count >= 10 || max_frames_count == 0)
            break;
        max_frames_count /= 2;
    }
    if (max_frames_count == 0)
        max_frames_count = 1;

    uint32_t size_packet_adc  = max_frames_count * work_channel_adc * sample_size / 2;
    info->size_packet_adc = size_packet_adc;
}

static struct zet017_device* zet017_get_device(struct zet017_server* server, uint32_t number) {
    if (server && server->device_count > number) {
        struct zet017_device* device = server->devices;
        while (device != NULL) {
            if (number == 0)
                return device;
            --number;
            device = device->next;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Сокеты                                                              */
/* ------------------------------------------------------------------ */

static socket_t zet017_socket_connect(const char* ip, unsigned short port) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock == INVALID_SOCKET)
        return INVALID_SOCKET;

    for (;;) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
            break;
        addr.sin_port = htons(port);

#if defined(ZET017_TCP_WINDOWS)
        u_long mode = 1;
        if (ioctlsocket(sock, FIONBIO, &mode) != 0)
#else
        int mode = 1;
        if (ioctl(sock, FIONBIO, &mode) < 0)
#endif
            break;

        int r = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
        if (r == SOCKET_ERROR) {
#if defined(ZET017_TCP_WINDOWS)
            int r = WSAGetLastError();
            if (r != WSAEWOULDBLOCK && r != WSAEINPROGRESS)
#else
            if (errno != EINPROGRESS)
#endif
                break;
        }
        return sock;
    }

    close_socket(sock);
    return INVALID_SOCKET;
}

static int zet017_socket_wait_connect(struct zet017_device* device, socket_t* sock) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(device->wakeup_socket[1], &rfds);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(*sock, &wfds);

    int nfds = (int)(*sock > device->wakeup_socket[1] ? *sock : device->wakeup_socket[1]);

    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;

    int r = select(nfds + 1, &rfds, &wfds, NULL, &tv);
    if (r != -1 && r != 0) {
        if (FD_ISSET(*sock, &wfds)) {
            int optval = 0;
            int optlen = sizeof(optval);
            r = getsockopt(*sock, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen);
            if (r == 0 && optval == 0) {
                optlen = sizeof(optval);
                optval = 1;
                (void)setsockopt(*sock, SOL_SOCKET, SO_KEEPALIVE, (const char*)&optval, optlen);
#if defined(TCP_KEEPALIVE)
                optval = 20;
                (void)setsockopt(*sock, IPPROTO_TCP, TCP_KEEPALIVE, (const char*)&optval, optlen);
#elif defined(TCP_KEEPIDLE)
                optval = 20;
                (void)setsockopt(*sock, IPPROTO_TCP, TCP_KEEPIDLE, (const char*)&optval, optlen);
#endif
#if defined(TCP_KEEPINTVL)
                optval = 1;
                (void)setsockopt(*sock, IPPROTO_TCP, TCP_KEEPINTVL, (const char*)&optval, optlen);
#endif
#if defined(TCP_KEEPCNT)
                optval = 10;
                (void)setsockopt(*sock, IPPROTO_TCP, TCP_KEEPCNT, (const char*)&optval, optlen);
#endif
                return 0;
            }
        }
        if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
            char buf;
            recv(device->wakeup_socket[1], &buf, 1, 0);
        }
    }
    return -1;
}

static int zet017_socket_handshake(struct zet017_device* device, socket_t* sock) {
    uint32_t flush_size = 0;
    char     flush_data[ZET017_MAX_FLUSH_SIZE + sizeof(flush_size)];
    int      flush_data_ptr = 0;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(device->wakeup_socket[1], &rfds);
        FD_SET(*sock, &rfds);

        int nfds = (int)(*sock > device->wakeup_socket[1] ? *sock : device->wakeup_socket[1]);

        struct timeval tv;
        tv.tv_sec  = 10;
        tv.tv_usec = 0;

        int r = select(nfds + 1, &rfds, NULL, NULL, &tv);
        if (r == -1 || r == 0)
            break;

        if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
            char buf;
            recv(device->wakeup_socket[1], &buf, 1, 0);
            break;
        }

        if (FD_ISSET(*sock, &rfds)) {
            int len = sizeof(flush_data) - flush_data_ptr;
            r = recv(*sock, flush_data + flush_data_ptr, len, 0);
            if (r <= 0)
                break;

            flush_data_ptr += r;
            if (flush_data_ptr >= (int)sizeof(flush_size)) {
                flush_size = *(uint32_t*)(flush_data);
                if (flush_data_ptr - (int)sizeof(flush_size) == (int)flush_size)
                    return 0;
            }
        }
    }
    return -1;
}

static int zet017_socket_cmd_connect(struct zet017_device* device) {
    for (;;) {
        device->cmd_socket = zet017_socket_connect(device->ip, ZET017_CMD_PORT);
        if (INVALID_SOCKET == device->cmd_socket) break;
        if (zet017_socket_wait_connect(device, &device->cmd_socket) != 0) break;
        if (zet017_socket_handshake(device, &device->cmd_socket)    != 0) break;
        return 0;
    }
    return -1;
}

static int zet017_socket_adc_connect(struct zet017_device* device) {
    for (;;) {
        device->adc_socket = zet017_socket_connect(device->ip, ZET017_ADC_PORT);
        if (INVALID_SOCKET == device->adc_socket) break;
        if (zet017_socket_wait_connect(device, &device->adc_socket) != 0) break;
        if (zet017_socket_handshake(device, &device->adc_socket)    != 0) break;
        return 0;
    }
    return -1;
}

static int zet017_socket_dac_connect(struct zet017_device* device) {
    for (;;) {
        device->dac_socket = zet017_socket_connect(device->ip, ZET017_DAC_PORT);
        if (INVALID_SOCKET == device->dac_socket) break;
        if (zet017_socket_wait_connect(device, &device->dac_socket) != 0) break;
        if (zet017_socket_handshake(device, &device->dac_socket)    != 0) break;
        return 0;
    }
    return -1;
}

static void zet017_wakeup_socket_cleanup(struct zet017_device* device) {
    if (device->wakeup_socket[0] != INVALID_SOCKET) {
        close_socket(device->wakeup_socket[0]);
        device->wakeup_socket[0] = INVALID_SOCKET;
    }
    if (device->wakeup_socket[1] != INVALID_SOCKET) {
        close_socket(device->wakeup_socket[1]);
        device->wakeup_socket[1] = INVALID_SOCKET;
    }
}

static int zet017_wakeup_socket_init(struct zet017_device* device) {
#if defined(ZET017_TCP_WINDOWS)
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET)
        return -1;

    for (;;) {
        int optval = 1;
        int optlen = sizeof(optval);
        (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (char*)&optval, optlen);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family           = AF_INET;
        addr.sin_addr.s_addr      = htonl(INADDR_LOOPBACK);
        addr.sin_port             = 0;
        if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) break;

        memset(&addr, 0, sizeof(addr));
        socklen_t addrlen = sizeof(addr);
        if (getsockname(listener, (struct sockaddr*)&addr, &addrlen) == SOCKET_ERROR) break;

        addr.sin_family           = AF_INET;
        addr.sin_addr.s_addr      = htonl(INADDR_LOOPBACK);
        if (listen(listener, 1) == SOCKET_ERROR) break;

        device->wakeup_socket[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (device->wakeup_socket[0] == INVALID_SOCKET) break;

        if (connect(device->wakeup_socket[0], (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) break;

        device->wakeup_socket[1] = accept(listener, NULL, NULL);
        if (device->wakeup_socket[1] == INVALID_SOCKET) break;

        closesocket(listener);
        return 0;
    }
    closesocket(listener);
#else
    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, device->wakeup_socket) == 0)
        return 0;
#endif

    zet017_wakeup_socket_cleanup(device);
    return -2;
}

static void zet017_device_wakeup(struct zet017_device* device) {
    char buf = 'x';
    send(device->wakeup_socket[0], &buf, sizeof(buf), 0);
}

static int zet017_device_process_wakeup(struct zet017_device* device) {
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(device->wakeup_socket[1], &rfds);

        struct timeval tv;
        tv.tv_sec  = 0;
        tv.tv_usec = 0;

        int r = select((int)device->wakeup_socket[1] + 1, &rfds, NULL, NULL, &tv);
        if (r == -1) return -1;
        if (r == 0)  return 0;

        if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
            char buf;
            recv(device->wakeup_socket[1], &buf, 1, 0);
        }
    }
    return 0;
}

static void zet017_device_close(struct zet017_device* device) {
    zet017_wakeup_socket_cleanup(device);
    if (device->cmd_socket != INVALID_SOCKET) {
        close_socket(device->cmd_socket);
        device->cmd_socket = INVALID_SOCKET;
    }
    if (device->adc_socket != INVALID_SOCKET) {
        close_socket(device->adc_socket);
        device->adc_socket = INVALID_SOCKET;
    }
    if (device->dac_socket != INVALID_SOCKET) {
        close_socket(device->dac_socket);
        device->dac_socket = INVALID_SOCKET;
    }
    device->is_connected = 0;
}

static void zet017_device_destroy(struct zet017_device* device) {
    device->running = 0;
    zet017_device_wakeup(device);
#if defined(ZET017_TCP_WINDOWS)
    WaitForSingleObject(device->work_thread, INFINITE);
    CloseHandle(device->work_thread);
#else
    pthread_join(device->work_thread, NULL);
#endif
    zet017_device_close(device);
    zet017_mutex_destroy(&device->state_mutex);
    zet017_mutex_destroy(&device->info_mutex);
    zet017_mutex_destroy(&device->config_mutex);
    zet017_mutex_destroy(&device->adc_data.mutex);
    zet017_mutex_destroy(&device->dac_data.mutex);
    zet017_mutex_destroy(&device->command.mutex);
    zet017_cond_destroy(&device->command.cond);
    free(device);
}

/* ------------------------------------------------------------------ */
/*  Команды устройства                                                  */
/* ------------------------------------------------------------------ */

static int zet017_device_wait_stop(struct zet017_device* device, union zet017_packet* packet) {
    int counter = 0;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(device->wakeup_socket[1], &rfds);
        FD_SET(device->adc_socket, &rfds);

        int nfds = (int)(device->adc_socket > device->wakeup_socket[1]
                         ? device->adc_socket : device->wakeup_socket[1]);

        struct timeval tv;
        tv.tv_sec  = 2;
        tv.tv_usec = 0;

        int r = select(nfds + 1, &rfds, NULL, NULL, &tv);
        if (r == -1 || r == 0) {
            zet017_device_close(device);
            break;
        }

        if (r > 0) {
            if (FD_ISSET(device->adc_socket, &rfds)) {
                r = recv(device->adc_socket, packet->raw, sizeof(*packet), 0);
                if (r <= 0) { zet017_device_close(device); break; }
                if (r == sizeof(*packet)) {
                    for (int i = 0; i < r; ++i) {
                        if (packet->raw[i] != 0) break;
                        if (i == r - 1) return 0;
                    }
                }
                if (++counter > 10) { zet017_device_close(device); break; }
            }
            if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
                char buf;
                if (recv(device->wakeup_socket[1], &buf, 1, 0) <= 0) {
                    zet017_device_close(device);
                    break;
                }
            }
        }
    }
    return -1;
}

static int zet017_device_process_command(struct zet017_device* device, union zet017_packet* packet) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(device->wakeup_socket[1], &rfds);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(device->cmd_socket, &wfds);

    int nfds = (int)(device->cmd_socket > device->wakeup_socket[1]
                     ? device->cmd_socket : device->wakeup_socket[1]);

    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;

    int r = select(nfds + 1, &rfds, &wfds, NULL, &tv);
    if (r == -1 || r == 0) return -1;

    if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
        char buf;
        recv(device->wakeup_socket[1], &buf, 1, 0);
        return -2;
    }

    if (FD_ISSET(device->cmd_socket, &wfds)) {
        r = send(device->cmd_socket, packet->raw, sizeof(*packet), 0);
        if (r != sizeof(*packet)) return -2;
    } else {
        return -3;
    }

    int data_ptr = 0;
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(device->wakeup_socket[1], &rfds);
        FD_SET(device->cmd_socket, &rfds);

        r = select(nfds + 1, &rfds, NULL, NULL, &tv);
        if (r == -1 || r == 0) break;

        if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
            char buf;
            recv(device->wakeup_socket[1], &buf, 1, 0);
            break;
        }

        if (FD_ISSET(device->cmd_socket, &rfds)) {
            int len = sizeof(*packet) - data_ptr;
            r = recv(device->cmd_socket, packet->raw + data_ptr, len, 0);
            if (r <= 0) break;
            data_ptr += r;
            if (data_ptr == sizeof(*packet)) return 0;
        }
    }
    return -4;
}

static void zet017_device_update_info(struct zet017_device* device, union zet017_packet* packet) {
    memcpy(&device->device_info, &packet->info, sizeof(struct zet017_device_info));

    device->adc_dac_data.sample_rate_adc = zet017_get_sample_rate_adc(device->device_info.mode_adc);
    device->adc_dac_data.work_channel_adc = device->device_info.work_channel_adc;
    device->adc_dac_data.sample_size_adc =
        (uint16_t)(device->device_info.type_data_adc == 0 ? sizeof(int16_t) : sizeof(int32_t));
    device->adc_dac_data.sample_rate_dac = zet017_get_sample_rate_dac(device->device_info.rate_dac);
    device->adc_dac_data.work_channel_dac = device->device_info.work_channel_dac;
    device->adc_dac_data.sample_size_dac =
        (uint16_t)(device->device_info.type_data_dac == 0 ? sizeof(int16_t) : sizeof(int32_t));

    zet017_mutex_lock(&device->info_mutex);
    strcpy(device->info.name, device->device_info.device_name);
    device->info.serial = device->device_info.serial;
    strcpy(device->info.version, device->device_info.version_dsp);
    zet017_mutex_unlock(&device->info_mutex);

    zet017_mutex_lock(&device->config_mutex);
    device->config.sample_rate_adc = zet017_get_sample_rate_adc(device->device_info.mode_adc);
    device->config.moda_adc        = device->device_info.mode_adc;
    device->config.sample_rate_dac = zet017_get_sample_rate_dac(device->device_info.rate_dac);
    device->config.rate_dac        = device->device_info.rate_dac;
    device->config.mask_channel_adc = device->device_info.mask_channel_adc;
    device->config.mask_icp         = device->device_info.mask_icp;
    for (uint32_t i = 0; i < 8; ++i) {
        device->config.gain[i]      = zet017_get_gain(device->device_info.amplify_code[i]);
        device->config.gain_code[i] = device->device_info.amplify_code[i];
    }
    if (device->device_info.quantity_channel_adc == 4) {
        device->config.mask_channel_adc =
            ((device->device_info.mask_channel_adc & 0x02) >> 1) +
            ((device->device_info.mask_channel_adc & 0x08) >> 2) +
            ((device->device_info.mask_channel_adc & 0x20) >> 3) +
            ((device->device_info.mask_channel_adc & 0x80) >> 4);
        device->config.mask_icp =
            ((device->device_info.mask_icp & 0x02) >> 1) +
            ((device->device_info.mask_icp & 0x08) >> 2) +
            ((device->device_info.mask_icp & 0x20) >> 3) +
            ((device->device_info.mask_icp & 0x80) >> 4);
        for (uint32_t i = 0; i < 4; ++i) {
            device->config.gain[i]      = zet017_get_gain(device->device_info.amplify_code[i * 2 + 1]);
            device->config.gain_code[i] = device->device_info.amplify_code[i * 2 + 1];
        }
    }
    device->config.builtin_dac_state       = device->device_info.builtin_dac_state;
    device->config.builtin_dac_sine_freq   = (double)device->device_info.builtin_dac_sine_freq;
    device->config.builtin_dac_sine_ampl   = (double)device->device_info.builtin_dac_sine_ampl;
    device->config.builtin_dac_sine_offset = (double)device->device_info.builtin_dac_sine_offset;
    if (device->device_info.resolution_dac[0]) {
        device->config.builtin_dac_sine_ampl   *= device->device_info.resolution_dac[0];
        device->config.builtin_dac_sine_offset *= device->device_info.resolution_dac[0];
    } else {
        device->config.builtin_dac_sine_ampl   *= device->device_info.resolution_dac_def;
        device->config.builtin_dac_sine_offset *= device->device_info.resolution_dac_def;
    }
    zet017_mutex_unlock(&device->config_mutex);

    zet017_mutex_lock(&device->state_mutex);
    uint32_t sample_size = (uint32_t)(device->device_info.type_data_adc == 0 ? sizeof(int16_t) : sizeof(int32_t));
    device->state.buffer_size_adc = ZET017_ADC_BUFFER_SIZE / sample_size / device->device_info.work_channel_adc;
    sample_size = (uint32_t)(device->device_info.type_data_dac == 0 ? sizeof(int16_t) : sizeof(int32_t));
    device->state.buffer_size_dac = ZET017_DAC_BUFFER_SIZE / sample_size;
    if (device->device_info.work_channel_dac != 0)
        device->state.buffer_size_dac /= device->device_info.work_channel_dac;
    zet017_mutex_unlock(&device->state_mutex);
}

static void zet017_device_update_tenso_info(struct zet017_device* device, union zet017_packet* packet) {
    memcpy(&device->tenso_info, packet->cmd.data.u8, sizeof(struct zet017_tenso_info));
    for (uint32_t i = 0; i < 8; ++i) {
        device->tenso_config.scheme[i]       = (enum zet017_scheme)device->tenso_info.scheme[i];
        device->tenso_config.correction_1[i] = device->tenso_info.correction[i][0];
        device->tenso_config.correction_2[i] = device->tenso_info.correction[i][1];
    }
}

static void zet017_device_update_adc_dac_info(struct zet017_device* device) {
    zet017_mutex_lock(&device->adc_data.mutex);

    device->adc_data.channel_quantity = device->device_info.quantity_channel_adc;
    device->adc_data.work_channel     = device->device_info.work_channel_adc;
    device->adc_data.channel_mask     = device->device_info.mask_channel_adc;
    device->adc_data.sample_size      = device->device_info.type_data_adc == 0
                                        ? sizeof(int16_t) : sizeof(int32_t);
    memcpy(device->adc_data.amplify_code, device->device_info.amplify_code,
           sizeof(device->device_info.amplify_code));
    if (device->device_info.quantity_channel_virt)
        device->adc_data.amplify_code[
            device->device_info.quantity_channel_adc -
            device->device_info.quantity_channel_virt] = 0;

    if (device->device_info.quantity_channel_adc == 4) {
        device->adc_data.channel_mask =
            ((device->device_info.mask_channel_adc & 0x02) >> 1) +
            ((device->device_info.mask_channel_adc & 0x08) >> 2) +
            ((device->device_info.mask_channel_adc & 0x20) >> 3) +
            ((device->device_info.mask_channel_adc & 0x80) >> 4);
        for (uint32_t i = 0; i < 4; ++i)
            device->adc_data.amplify_code[i] = device->device_info.amplify_code[i * 2 + 1];
    }

    uint16_t qty = device->device_info.quantity_channel_adc - device->device_info.quantity_channel_virt;
    for (uint32_t i = 0; i < qty; ++i) {
        uint32_t* dummy = (uint32_t*)device->correction.amplify[i];
        if (*dummy == 0) {
            float resolution = device->device_info.resolution_adc_def;
            dummy = (uint32_t*)(device->device_info.resolution_adc + i);
            if (qty == 4)
                dummy = (uint32_t*)(device->device_info.resolution_adc + i + i + 1);
            if (*dummy != 0)
                resolution = *(float*)dummy;
            device->adc_data.resolution[i][0] = resolution;
            device->adc_data.resolution[i][1] = resolution / 10.f;
            device->adc_data.resolution[i][2] = resolution / 100.f;
        } else {
            device->adc_data.resolution[i][0] = *(float*)dummy;
            device->adc_data.resolution[i][1] = device->adc_data.resolution[i][0] / device->correction.amplify[i][1];
            device->adc_data.resolution[i][2] = device->adc_data.resolution[i][0] / device->correction.amplify[i][2];
        }
    }
    if (device->device_info.quantity_channel_virt) {
        uint32_t* dummy = (uint32_t*)(device->correction.reduction);
        if (*dummy == 0) {
            float resolution = device->device_info.resolution_dac_def;
            dummy = (uint32_t*)(device->device_info.resolution_dac);
            if (*dummy != 0)
                resolution = *(float*)dummy;
            device->adc_data.resolution[qty][0] = resolution;
        } else {
            device->adc_data.resolution[qty][0] = *(float*)dummy;
        }
    }

    zet017_mutex_unlock(&device->adc_data.mutex);

    zet017_mutex_lock(&device->dac_data.mutex);

    device->dac_data.channel_quantity = device->device_info.work_channel_dac;
    device->dac_data.channel_mask     = device->device_info.mask_channel_dac;
    device->dac_data.sample_size      = device->device_info.type_data_dac == 0
                                        ? sizeof(int16_t) : sizeof(int32_t);

    uint16_t qty_dac = device->device_info.quantity_channel_dac;
    for (uint32_t i = 0; i < qty_dac; ++i) {
        uint32_t* dummy = (uint32_t*)(device->correction.reduction + i);
        if (*dummy == 0) {
            float resolution = device->device_info.resolution_dac_def;
            dummy = (uint32_t*)(device->device_info.resolution_dac + i);
            if (*dummy != 0)
                resolution = *(float*)dummy;
            device->dac_data.resolution[i] = resolution;
        } else {
            device->dac_data.resolution[i] = *(float*)dummy;
        }
    }

    zet017_mutex_unlock(&device->dac_data.mutex);
}

static int zet017_device_get_info_cmd(struct zet017_device* device, union zet017_packet* packet) {
    memset(packet, 0x0, sizeof(*packet));
    packet->info.command = ZET017_CMD_GET_INFO;
    if (zet017_device_process_command(device, packet) != 0)
        return -1;
    zet017_device_update_info(device, packet);
    return 0;
}

static int zet017_device_put_info_cmd(struct zet017_device* device, union zet017_packet* packet) {
    packet->info.command = ZET017_CMD_PUT_INFO;
    if (0 != zet017_device_process_command(device, packet))
        return -1;
    zet017_device_update_info(device, packet);
    return 0;
}

static int zet017_device_start_cmd(struct zet017_device* device, union zet017_packet* packet) {
    packet->info.command = ZET017_CMD_PUT_INFO;
    if (0 != zet017_device_process_command(device, packet))
        return -1;

    memset(device->adc_data.buffer, 0x0, ZET017_ADC_BUFFER_SIZE);
    device->adc_data.pointer      = 0;
    device->adc_data.frames_total = 0;
    memset(device->dac_data.buffer, 0x0, ZET017_DAC_BUFFER_SIZE);
    device->dac_data.pointer = 0;
    device->adc_dac_data.adc_count = 0;
    device->adc_dac_data.dac_count = 0;

    zet017_device_update_info(device, packet);
    return 0;
}

static int zet017_device_stop_cmd(struct zet017_device* device, union zet017_packet* packet) {
    for (;;) {
        if (device->device_info.start_adc == 0)
            return 0;

        memcpy(&packet->info, &device->device_info, sizeof(struct zet017_device_info));
        packet->info.command  = ZET017_CMD_PUT_INFO;
        packet->info.start_adc = -1;
        if (packet->info.start_dac != 0)
            packet->info.start_dac = -1;
        if (0 != zet017_device_process_command(device, packet)) break;
        if (0 != zet017_device_wait_stop(device, packet)) break;

        memcpy(&packet->info, &device->device_info, sizeof(struct zet017_device_info));
        packet->info.command   = ZET017_CMD_PUT_INFO;
        packet->info.start_adc = 0;
        packet->info.start_dac = 0;
        if (0 != zet017_device_process_command(device, packet)) break;

        zet017_device_update_info(device, packet);
        return 0;
    }
    return -1;
}

static int zet017_device_read_correction_cmd(struct zet017_device* device, union zet017_packet* packet) {
    for (;;) {
        memset(&packet->cmd, 0x0, sizeof(struct zet017_command_info));
        packet->cmd.command = ZET017_CMD_READ_CORRECTION;
        packet->cmd.error   = 1;
        packet->cmd.size    = sizeof(struct zet017_correction_info);
        if (0 != zet017_device_process_command(device, packet)) break;

        if (packet->cmd.command == ZET017_CMD_READ_CORRECTION)
            memcpy(&device->correction, packet->cmd.data.u8, sizeof(struct zet017_correction_info));
        else
            memset(&device->correction, 0x0, sizeof(struct zet017_correction_info));

        return 0;
    }
    return -1;
}

static int zet017_device_read_tenso_cmd(struct zet017_device* device, union zet017_packet* packet) {
    for (;;) {
        memset(&packet->cmd, 0x0, sizeof(struct zet017_command_info));
        packet->cmd.command = ZET017_CMD_READ_TENSO;
        packet->cmd.error   = 1;
        packet->cmd.size    = sizeof(struct zet017_tenso_info);
        if (0 != zet017_device_process_command(device, packet)) break;

        if (packet->cmd.command == ZET017_CMD_READ_TENSO)
            zet017_device_update_tenso_info(device, packet);
        else
            memset(&device->tenso_info, 0x0, sizeof(struct zet017_tenso_info));

        return 0;
    }
    return -1;
}

static int zet017_device_write_tenso_cmd(struct zet017_device* device, union zet017_packet* packet) {
    packet->cmd.command = ZET017_CMD_WRITE_TENSO;
    packet->cmd.error   = 1;
    packet->cmd.size    = sizeof(struct zet017_tenso_info);
    if (0 != zet017_device_process_command(device, packet))
        return -1;
    zet017_device_update_tenso_info(device, packet);
    return 0;
}

static const char* zet017_command_name(enum zet017_command command) {
    switch (command) {
    case zet017_set_config:          return "set_config";
    case zet017_write_tenso_config:  return "write_tenso_config";
    case zet017_start:               return "start";
    case zet017_stop:                return "stop";
    default:                         break;
    }
    return "unknown";
}

static int zet017_device_connect(struct zet017_device* device) {
    const char* stage = "wakeup socket";
    for (;;) {
        if (zet017_wakeup_socket_init(device) != 0) break;
        stage = "cmd socket";
        if (zet017_socket_cmd_connect(device)  != 0) break;
        stage = "adc socket";
        if (zet017_socket_adc_connect(device)  != 0) break;
        stage = "dac socket";
        if (zet017_socket_dac_connect(device)  != 0) break;

        memset(device->adc_data.buffer, 0x0, ZET017_ADC_BUFFER_SIZE);
        device->adc_data.pointer      = 0;
        device->adc_data.frames_total = 0;
        memset(device->dac_data.buffer, 0x0, ZET017_DAC_BUFFER_SIZE);
        device->dac_data.pointer = 0;
        device->adc_dac_data.adc_count = 0;
        device->adc_dac_data.dac_count = 0;

        return 0;
    }
    DLOG(device, zet017_cat_network, zet017_log_warning,
         "connect failed at %s (ip=%s, err=%d)", stage, device->ip, zet017_last_error());
    zet017_device_close(device);
    return -1;
}

static int zet017_device_init(struct zet017_device* device, union zet017_packet* packet) {
    for (;;) {
        if (zet017_device_get_info_cmd(device, packet) != 0) break;

        packet->info.start_adc = packet->info.start_dac = 0;
        zet017_set_size_packet_adc(&packet->info);

        if (zet017_device_put_info_cmd(device, packet)       != 0) break;
        if (zet017_device_read_correction_cmd(device, packet) != 0) break;
        if (zet017_device_read_tenso_cmd(device, packet)      != 0) break;

        zet017_device_update_adc_dac_info(device);
        device->timestamp = zet017_get_timestamp();
        return 0;
    }
    DLOG(device, zet017_cat_network, zet017_log_warning,
         "device init failed (ip=%s, err=%d)", device->ip, zet017_last_error());
    zet017_device_close(device);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Обработка потока АЦП/ЦАП                                            */
/* ------------------------------------------------------------------ */

static void zet017_process_adc_dac(struct zet017_device* device, union zet017_packet* packet) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(device->wakeup_socket[1], &rfds);
    FD_SET(device->adc_socket, &rfds);
    FD_SET(device->dac_socket, &rfds);

    int nfds = (int)(device->adc_socket > device->wakeup_socket[1]
                     ? device->adc_socket : device->wakeup_socket[1]);
    if (nfds < (int)device->dac_socket)
        nfds = (int)device->dac_socket;

    int dac = 0;
    fd_set wfds;
    FD_ZERO(&wfds);
    if (device->device_info.start_dac) {
        uint64_t dac_count =
            device->adc_dac_data.adc_count *
            device->adc_dac_data.sample_rate_dac /
            device->adc_dac_data.sample_rate_adc;
        if (device->adc_dac_data.dac_count < dac_count + device->adc_dac_data.sample_rate_dac / 5)
            dac = 1;
    }
    if (dac != 0)
        FD_SET(device->dac_socket, &wfds);

    struct timeval tv;
    tv.tv_sec  = 10;
    tv.tv_usec = 0;

    int r = select(nfds + 1, &rfds, dac ? &wfds : NULL, NULL, &tv);
    if (r == -1) {
        DLOG(device, zet017_cat_network, zet017_log_warning,
             "stream select failed (ip=%s, err=%d)", device->ip, zet017_last_error());
        zet017_device_close(device);
        return;
    }

    if (r > 0) {
        if (FD_ISSET(device->adc_socket, &rfds)) {
            r = recv(device->adc_socket, packet->raw, sizeof(*packet), 0);
            if (r <= 0) {
                DLOG(device, zet017_cat_network, zet017_log_warning,
                     "adc socket closed (ip=%s, recv=%d, err=%d)",
                     device->ip, r, zet017_last_error());
                zet017_device_close(device);
                return;
            }
            if (r == sizeof(*packet)) {
                zet017_mutex_lock(&device->adc_data.mutex);

                uint32_t size = device->device_info.size_packet_adc * 2;
                uint32_t frames =
                    size / device->adc_dac_data.work_channel_adc / device->adc_dac_data.sample_size_adc;
                device->adc_dac_data.adc_count  += frames;
                device->adc_data.frames_total   += frames;

                if (size <= ZET017_ADC_BUFFER_SIZE - device->adc_data.pointer) {
                    memcpy(device->adc_data.buffer + device->adc_data.pointer, packet->raw, size);
                    device->adc_data.pointer += size;
                    if (device->adc_data.pointer >= ZET017_ADC_BUFFER_SIZE)
                        device->adc_data.pointer -= ZET017_ADC_BUFFER_SIZE;
                } else {
                    uint32_t part1 = ZET017_ADC_BUFFER_SIZE - device->adc_data.pointer;
                    memcpy(device->adc_data.buffer + device->adc_data.pointer, packet->raw, part1);
                    uint32_t part2 = size - part1;
                    memcpy(device->adc_data.buffer, packet->raw + part1, part2);
                    device->adc_data.pointer = part2;
                }

                zet017_mutex_unlock(&device->adc_data.mutex);
            }
        }

        if (FD_ISSET(device->dac_socket, &rfds)) {
            r = recv(device->dac_socket, packet->raw, sizeof(*packet), 0);
            if (r <= 0) {
                DLOG(device, zet017_cat_network, zet017_log_warning,
                     "dac socket closed (ip=%s, recv=%d, err=%d)",
                     device->ip, r, zet017_last_error());
                zet017_device_close(device);
                return;
            }
        }

        if (dac != 0 && FD_ISSET(device->dac_socket, &wfds)) {
            uint32_t size = sizeof(*packet);

            zet017_mutex_lock(&device->dac_data.mutex);

            if (size <= ZET017_DAC_BUFFER_SIZE - device->dac_data.pointer) {
                memcpy(packet->raw, device->dac_data.buffer + device->dac_data.pointer, size);
                memset(device->dac_data.buffer + device->dac_data.pointer, 0, size);
                device->dac_data.pointer += size;
                if (device->dac_data.pointer >= ZET017_DAC_BUFFER_SIZE)
                    device->dac_data.pointer -= ZET017_DAC_BUFFER_SIZE;
            } else {
                uint32_t part1 = ZET017_DAC_BUFFER_SIZE - device->dac_data.pointer;
                memcpy(packet->raw, device->dac_data.buffer + device->dac_data.pointer, part1);
                memset(device->dac_data.buffer + device->dac_data.pointer, 0, part1);
                uint32_t part2 = size - part1;
                memcpy(packet->raw + part1, device->dac_data.buffer, part2);
                device->dac_data.pointer = part2;
            }

            zet017_mutex_unlock(&device->dac_data.mutex);

            r = send(device->dac_socket, packet->raw, sizeof(*packet), 0);
            if (r != sizeof(*packet)) {
                DLOG(device, zet017_cat_network, zet017_log_warning,
                     "dac send incomplete (ip=%s, sent=%d, expected=%d, err=%d)",
                     device->ip, r, (int)sizeof(*packet), zet017_last_error());
                zet017_device_close(device);
                return;
            }
            device->adc_dac_data.dac_count +=
                sizeof(*packet) / device->adc_dac_data.work_channel_dac / device->adc_dac_data.sample_size_dac;
        }

        if (FD_ISSET(device->wakeup_socket[1], &rfds)) {
            char buf;
            if (recv(device->wakeup_socket[1], &buf, 1, 0) <= 0) {
                DLOG(device, zet017_cat_network, zet017_log_warning,
                     "wakeup socket closed (ip=%s, err=%d)", device->ip, zet017_last_error());
                zet017_device_close(device);
                return;
            }
        }
    }
}

static void zet017_update_state(struct zet017_device* device, union zet017_packet* packet) {
    uint32_t timestamp = zet017_get_timestamp();

    /* Раз в 10 секунд — темп приёма. Служит точкой отсчёта при разборе
     * сообщений о пропусках в данных вместе с adc buffer overrun. */
    if (timestamp - device->log_rate_ts >= 10000) {
        device->log_rate_ts = timestamp;
        DLOG(device, zet017_cat_data, zet017_log_info,
             "adc stream (ip=%s, frames=%llu, pointer=%u, reconnect=%u)",
             device->ip, (unsigned long long)device->adc_dac_data.adc_count,
             device->adc_data.pointer, device->reconnect);
    }

    if (timestamp - device->timestamp > 60000) {
        device->timestamp = timestamp;
        if (zet017_device_get_info_cmd(device, packet) != 0) {
            zet017_device_close(device);
            return;
        }
    }

    zet017_mutex_lock(&device->state_mutex);

    device->state.is_connected = device->is_connected;
    device->state.reconnect    = device->reconnect;
    device->state.pointer_adc  = device->adc_data.pointer / device->device_info.work_channel_adc;
    if (device->device_info.type_data_adc == 0)
        device->state.pointer_adc /= sizeof(int16_t);
    if (device->device_info.type_data_adc == 1)
        device->state.pointer_adc /= sizeof(int32_t);
    if (device->device_info.work_channel_dac != 0)
        device->state.pointer_dac = device->dac_data.pointer / device->device_info.work_channel_dac;
    else
        device->state.pointer_dac = 0;
    if (device->device_info.type_data_dac == 0)
        device->state.pointer_dac /= sizeof(int16_t);
    if (device->device_info.type_data_dac == 1)
        device->state.pointer_dac /= sizeof(int32_t);

    zet017_mutex_unlock(&device->state_mutex);
}

static void zet017_process_command(struct zet017_device* device) {
    zet017_mutex_lock(&device->command.mutex);

    if (device->command.state == zet017_command_idle) {
        zet017_mutex_unlock(&device->command.mutex);
        return;
    }

    device->command.state  = zet017_command_processing;
    device->command.result = -1;

    if (zet017_device_process_wakeup(device) == 0) {
        switch (device->command.command) {
        case zet017_set_config:
            device->command.result = zet017_device_put_info_cmd(device, &device->command.data);
            zet017_device_update_adc_dac_info(device);
            break;
        case zet017_write_tenso_config:
            device->command.result = zet017_device_write_tenso_cmd(device, &device->command.data);
            break;
        case zet017_start:
            device->command.result = zet017_device_start_cmd(device, &device->command.data);
            zet017_device_update_adc_dac_info(device);
            break;
        case zet017_stop:
            device->command.result = zet017_device_stop_cmd(device, &device->command.data);
            break;
        default:
            break;
        }
    }

    device->command.state = zet017_command_completed;
    if (device->command.result != 0)
        zet017_device_close(device);

    /* Правило блокировок: под command.mutex не логируем */
    enum zet017_command done_command = device->command.command;
    int                 done_result  = device->command.result;

    zet017_cond_signal(&device->command.cond);
    zet017_mutex_unlock(&device->command.mutex);

    if (done_result != 0) {
        DLOG(device, zet017_cat_command, zet017_log_error,
             "command failed (ip=%s, command=%s, result=%d, err=%d), connection dropped",
             device->ip, zet017_command_name(done_command), done_result, zet017_last_error());
    } else {
        DLOG(device, zet017_cat_command, zet017_log_info,
             "command done (ip=%s, command=%s)",
             device->ip, zet017_command_name(done_command));
    }
}

/* ------------------------------------------------------------------ */
/*  Поток устройства                                                    */
/* ------------------------------------------------------------------ */

static THREAD_RETURN zet017_device_thread_func(void* arg) {
    struct zet017_device* device = (struct zet017_device*)arg;
    union zet017_packet   packet;
#if !defined(ZET017_TCP_WINDOWS)
    struct timespec ts = { 0, 100000000 };
#endif

    while (device->running) {
        if (device->is_connected) {
            zet017_process_adc_dac(device, &packet);
        } else {
            if (zet017_device_connect(device) == 0) {
                if (zet017_device_init(device, &packet) == 0) {
                    device->is_connected = 1;
                    ++device->reconnect;
                    DLOG(device, zet017_cat_network, zet017_log_info,
                         "connected (ip=%s, name=%s, serial=%u, reconnect #%u)",
                         device->ip, device->device_info.device_name,
                         device->device_info.serial, device->reconnect);
                }
            }
            if (!device->is_connected) {
#if defined(ZET017_TCP_WINDOWS)
                Sleep(100);
#else
                nanosleep(&ts, NULL);
#endif
                continue;
            }
        }

        zet017_process_command(device);
        zet017_update_state(device, &packet);
    }

#if defined(ZET017_TCP_WINDOWS)
    return 0;
#else
    return NULL;
#endif
}

/* ================================================================== */
/*  П У Б Л И Ч Н Ы Й   A P I  (handle-based)                        */
/*  LabView видит только int32_t, примитивы и POD-структуры            */
/* ================================================================== */

ZET017_TCP_API zet017_server_create(zet017_handle* handle, const char* log_path) {
    return zet017_server_create_ex(handle, log_path, zet017_log_info);
}

ZET017_TCP_API zet017_server_create_ex(zet017_handle* handle, const char* log_path,
                                       zet017_int32 level)
{
    int lvl = (int)level;
    if (lvl < zet017_log_off)  lvl = zet017_log_off;
    if (lvl > zet017_log_info) lvl = zet017_log_info;

    /* Сервера ещё нет: отказы до его создания пишет early-логгер,
     * штатное "entry" записывается уже через готовый логгер — чтобы баннер
     * сессии оставался первой строкой файла. */
    if (!handle) {
        zet017_log_early(log_path, lvl, zet017_log_warning,
                         "zet017_server_create: handle is NULL -> return -1");
        return -1;
    }

    *handle = 0;

    registry_init();

    if (network_init() != 0) {
        zet017_log_early(log_path, lvl, zet017_log_error,
                         "zet017_server_create: network_init failed -> return -2");
        return -2;
    }

    struct zet017_server* server = malloc(sizeof(struct zet017_server));
    if (!server) {
        zet017_log_early(log_path, lvl, zet017_log_error,
                         "zet017_server_create: malloc failed -> return -3");
        network_cleanup();
        return -3;
    }

    memset(server, 0, sizeof(struct zet017_server));

    /* Логгер инициализируется НА МЕСТЕ, внутри уже выделенного сервера:
     * mutex_t (CRITICAL_SECTION / pthread_mutex_t) нельзя копировать
     * побайтово, поэтому вариант «собрать на стеке и перенести» непригоден. */
    if (zet017_logger_init(&server->logger, log_path, lvl) != 0) {
        zet017_log_early(log_path, lvl, zet017_log_error,
                         "zet017_server_create: logger mutex_init failed -> return -4");
        free(server);
        network_cleanup();
        return -4;
    }

    SLOG(server, zet017_cat_api, zet017_log_info, "zet017_server_create: entry");

    if (zet017_mutex_init(&server->devices_mutex) != 0) {
        SLOG(server, zet017_cat_api, zet017_log_error,
             "zet017_server_create: mutex_init failed -> return -4");
        zet017_logger_destroy(&server->logger);
        free(server);
        network_cleanup();
        return -4;
    }

    zet017_handle h = registry_add(server);
    if (h == 0) {
        SLOG(server, zet017_cat_api, zet017_log_error,
             "zet017_server_create: registry_add failed (no free slot) -> return -5");
        zet017_mutex_destroy(&server->devices_mutex);
        zet017_logger_destroy(&server->logger);
        free(server);
        network_cleanup();
        return -5;
    }

    *handle = h;
    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_create: success (handle=%d, level=%s)",
         (int)h, zet017_log_level_name(lvl));
    return 0;
}

ZET017_TCP_API zet017_server_set_log_level(zet017_handle handle, zet017_int32 level) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    if (level < zet017_log_off || level > zet017_log_info) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_server_set_log_level: invalid level (%d) -> return -3", (int)level);
        return -3;
    }

    for (int i = 0; i < zet017_cat_count; ++i)
        server->logger.level[i] = (int)level;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_set_log_level: all categories -> %s",
         zet017_log_level_name((int)level));
    return 0;
}

ZET017_TCP_API zet017_server_set_log_category_level(zet017_handle handle,
                                                    zet017_int32 category,
                                                    zet017_int32 level)
{
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    if (category < 0 || category >= zet017_cat_count) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_server_set_log_category_level: invalid category (%d) -> return -2",
             (int)category);
        return -2;
    }
    if (level < zet017_log_off || level > zet017_log_info) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_server_set_log_category_level: invalid level (%d) -> return -3",
             (int)level);
        return -3;
    }

    server->logger.level[category] = (int)level;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_set_log_category_level: %s -> %s",
         zet017_log_cat_tag((int)category), zet017_log_level_name((int)level));
    return 0;
}

ZET017_TCP_API zet017_server_set_log_file(zet017_handle handle, const char* log_path) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;
    if (!server->logger.ready) return -4;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_set_log_file: switching log file");

    zet017_mutex_lock(&server->logger.mutex);
    int r = zet017_logger_open_locked(&server->logger, log_path);
    zet017_mutex_unlock(&server->logger.mutex);

    if (r != 0) {
        /* Прежний файл остался открытым — состояние не изменилось */
        SLOG(server, zet017_cat_api, zet017_log_error,
             "zet017_server_set_log_file: cannot open file (err=%d) -> return -4",
             zet017_last_error());
        return -4;
    }
    return 0;
}

ZET017_TCP_API zet017_server_free(zet017_handle* handle) {
    if (!handle) return -1;

    struct zet017_server* server = registry_get(*handle);
    /* Логировать некуда: без сервера нет и логгера */
    if (!server) return -2;

    SLOG(server, zet017_cat_api, zet017_log_info, "zet017_server_free: entry");

    registry_remove(*handle);
    *handle = 0;

    zet017_mutex_lock(&server->devices_mutex);
    struct zet017_device* current = server->devices;
    while (current != NULL) {
        struct zet017_device* next = current->next;
        zet017_device_destroy(current);
        current = next;
    }
    server->devices      = NULL;
    server->device_count = 0;
    zet017_mutex_unlock(&server->devices_mutex);
    zet017_mutex_destroy(&server->devices_mutex);

    /* Логируем перед финальным free — после нельзя */
    SLOG(server, zet017_cat_api, zet017_log_info, "zet017_server_free: success");

    /* Логгер уничтожается ПОСЛЕДНИМ, после финального сообщения */
    zet017_logger_destroy(&server->logger);

    free(server);
    network_cleanup();

    return 0;
}

ZET017_TCP_API zet017_server_add_device(zet017_handle handle, const char* ip) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;
    if (!ip)     return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_add_device: entry (ip=%s)", ip);

    zet017_mutex_lock(&server->devices_mutex);

    struct zet017_device* existing = server->devices;
    while (existing != NULL) {
        if (strcmp(existing->ip, ip) == 0) {
            zet017_mutex_unlock(&server->devices_mutex);
            SLOG(server, zet017_cat_api, zet017_log_warning,
         "zet017_server_add_device: duplicate IP (ip=%s) -> return -3", ip);
            return -3;
        }
        existing = existing->next;
    }

    struct zet017_device* device = malloc(sizeof(struct zet017_device));
    if (!device) {
        zet017_mutex_unlock(&server->devices_mutex);
        SLOG(server, zet017_cat_api, zet017_log_error,
         "zet017_server_add_device: malloc failed (ip=%s) -> return -4", ip);
        return -4;
    }

    for (;;) {
        memset(device, 0, sizeof(struct zet017_device));
        /* Владелец должен быть проставлен ДО запуска рабочего потока:
         * поток получает отсюда логгер с первой же итерации. */
        device->owner = server;
        strncpy(device->ip,       ip, MAX_IP_LENGTH - 1);
        strncpy(device->info.ip,  ip, MAX_IP_LENGTH - 1);
        device->ip[MAX_IP_LENGTH - 1]      = '\0';
        device->info.ip[MAX_IP_LENGTH - 1] = '\0';

        device->cmd_socket = device->adc_socket = device->dac_socket = INVALID_SOCKET;
        device->wakeup_socket[0] = device->wakeup_socket[1] = INVALID_SOCKET;
        device->is_connected = 0;

        if (zet017_mutex_init(&device->state_mutex)    != 0) break;
        if (zet017_mutex_init(&device->info_mutex)     != 0) break;
        if (zet017_mutex_init(&device->config_mutex)   != 0) break;
        if (zet017_mutex_init(&device->command.mutex)  != 0) break;
        if (zet017_mutex_init(&device->adc_data.mutex) != 0) break;
        if (zet017_mutex_init(&device->dac_data.mutex) != 0) break;
        if (zet017_cond_init(&device->command.cond)    != 0) break;

        device->command.state = zet017_command_idle;
        device->running       = 1;

#if defined(ZET017_TCP_WINDOWS)
        device->work_thread = CreateThread(
            NULL, 0, (LPTHREAD_START_ROUTINE)zet017_device_thread_func, device, 0, NULL);
        if (device->work_thread == NULL)
#else
        if (pthread_create(&device->work_thread, NULL, zet017_device_thread_func, device) != 0)
#endif
            break;

        if (server->devices == NULL) {
            server->devices = device;
        } else {
            for (struct zet017_device* d = server->devices; d != NULL; d = d->next) {
                if (d->next == NULL) { d->next = device; break; }
            }
        }
        ++server->device_count;

        zet017_mutex_unlock(&server->devices_mutex);
        SLOG(server, zet017_cat_api, zet017_log_info,
             "zet017_server_add_device: success (ip=%s)", ip);
        return 0;
    }

    zet017_mutex_unlock(&server->devices_mutex);
    SLOG(server, zet017_cat_api, zet017_log_error,
         "zet017_server_add_device: thread/mutex init failed (ip=%s) -> return -5", ip);
    zet017_device_destroy(device);
    return -5;
}

ZET017_TCP_API zet017_server_remove_device(zet017_handle handle, const char* ip) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;
    if (!ip)     return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_remove_device: entry (ip=%s)", ip);

    zet017_mutex_lock(&server->devices_mutex);

    struct zet017_device* current = server->devices;
    struct zet017_device* prev    = NULL;
    while (current != NULL) {
        if (strcmp(current->ip, ip) == 0) {
            if (prev == NULL)
                server->devices = current->next;
            else
                prev->next = current->next;

            zet017_device_destroy(current);
            --server->device_count;

            zet017_mutex_unlock(&server->devices_mutex);
            SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_server_remove_device: success (ip=%s)", ip);
            return 0;
        }
        prev    = current;
        current = current->next;
    }

    zet017_mutex_unlock(&server->devices_mutex);
    SLOG(server, zet017_cat_api, zet017_log_warning,
         "zet017_server_remove_device: device not found (ip=%s) -> return -1", ip);
    return -1;
}

ZET017_TCP_API zet017_device_get_info(zet017_handle handle, uint32_t number, struct zet017_info* info) {
    if (!info) return -1;

    struct zet017_server* server = registry_get(handle);
    if (!server) return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_info: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_get_info: device not found (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->info_mutex);
    memcpy(info, &device->info, sizeof(struct zet017_info));
    zet017_mutex_unlock(&device->info_mutex);

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_info: success (number=%u)", number);
    return 0;
}

ZET017_TCP_API zet017_device_get_state(zet017_handle handle, uint32_t number, struct zet017_state* state) {
    if (!state) return -1;

    struct zet017_server* server = registry_get(handle);
    if (!server) return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_state: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_get_state: device not found (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->state_mutex);
    memcpy(state, &device->state, sizeof(struct zet017_state));
    zet017_mutex_unlock(&device->state_mutex);

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_state: success (number=%u)", number);
    return 0;
}

ZET017_TCP_API zet017_device_get_config(zet017_handle handle, uint32_t number, struct zet017_config* config) {
    if (!config) return -1;

    struct zet017_server* server = registry_get(handle);
    if (!server) return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_config: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_get_config: device not found (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->config_mutex);
    memcpy(config, &device->config, sizeof(struct zet017_config));
    zet017_mutex_unlock(&device->config_mutex);

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_config: success (number=%u)", number);
    return 0;
}

ZET017_TCP_API zet017_device_get_tenso_config(zet017_handle handle, uint32_t number, struct zet017_tenso_config* config) {
    if (!config) return -1;

    struct zet017_server* server = registry_get(handle);
    if (!server) return -2;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_tenso_config: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_get_tenso_config: device not found (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->config_mutex);
    memcpy(config, &device->tenso_config, sizeof(struct zet017_tenso_config));
    zet017_mutex_unlock(&device->config_mutex);

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_get_tenso_config: success (number=%u)", number);
    return 0;
}

ZET017_TCP_API zet017_device_set_config(zet017_handle handle, uint32_t number, const struct zet017_config* config) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_set_config: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_config: device not found (number=%u) -> return -1", number);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_config: not connected (number=%u) -> return -2", number);
        return -2;
    }

    if (!config) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_config: config NULL (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->command.mutex);

    memcpy(&device->command.data.info, &device->device_info, sizeof(struct zet017_device_info));

    if (config->sample_rate_adc)
        device->command.data.info.mode_adc = zet017_get_mode_adc(config->sample_rate_adc);
    else
        device->command.data.info.mode_adc = config->moda_adc;

    if (config->sample_rate_dac)
        device->command.data.info.rate_dac = zet017_get_rate_dac(config->sample_rate_dac);
    else
        device->command.data.info.rate_dac = config->rate_dac;

    device->command.data.info.mask_channel_adc = config->mask_channel_adc;
    device->command.data.info.mask_icp         = config->mask_icp;

    for (uint32_t i = 0; i < 8; ++i) {
        if (config->gain[i])
            device->command.data.info.amplify_code[i] = zet017_get_amplify_code(config->gain[i]);
        else
            device->command.data.info.amplify_code[i] = config->gain_code[i];
    }

    if (device->command.data.info.quantity_channel_adc == 4) {
        device->command.data.info.mask_channel_adc =
            ((config->mask_channel_adc & 0x1) << 1) +
            ((config->mask_channel_adc & 0x2) << 2) +
            ((config->mask_channel_adc & 0x4) << 3) +
            ((config->mask_channel_adc & 0x8) << 4);
        device->command.data.info.mask_icp =
            ((config->mask_icp & 0x1) << 1) +
            ((config->mask_icp & 0x2) << 2) +
            ((config->mask_icp & 0x4) << 3) +
            ((config->mask_icp & 0x8) << 4);
        for (uint32_t i = 0; i < 8; ++i) {
            if (config->gain[i / 2])
                device->command.data.info.amplify_code[i] = zet017_get_amplify_code(config->gain[i / 2]);
            else
                device->command.data.info.amplify_code[i] = config->gain_code[i / 2];
        }
    }

    device->command.data.info.builtin_dac_state     = config->builtin_dac_state;
    device->command.data.info.builtin_dac_sine_freq = (int32_t)config->builtin_dac_sine_freq;

    double resolution_dac = device->command.data.info.resolution_dac[0];
    if (!resolution_dac)
        resolution_dac = device->command.data.info.resolution_dac_def;
    device->command.data.info.builtin_dac_sine_ampl   = (int32_t)(config->builtin_dac_sine_ampl   / resolution_dac);
    device->command.data.info.builtin_dac_sine_offset = (int32_t)(config->builtin_dac_sine_offset / resolution_dac);

    zet017_set_size_packet_adc(&device->command.data.info);

    device->command.command = zet017_set_config;
    device->command.result  = 0;
    device->command.state   = zet017_command_requested;
    zet017_device_wakeup(device);

    while (device->command.state != zet017_command_completed)
        zet017_cond_wait(&device->command.cond, &device->command.mutex);

    device->command.state = zet017_command_idle;
    int r = device->command.result;

    zet017_mutex_unlock(&device->command.mutex);
    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_set_config: done (number=%u)", number);
    return r;
}

ZET017_TCP_API zet017_device_set_tenso_config(zet017_handle handle, uint32_t number, const struct zet017_tenso_config* config) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_set_tenso_config: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_tenso_config: device not found (number=%u) -> return -1", number);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_tenso_config: not connected (number=%u) -> return -2", number);
        return -2;
    }

    if (!config) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_set_tenso_config: config NULL (number=%u) -> return -3", number);
        return -3;
    }

    zet017_mutex_lock(&device->command.mutex);

    memcpy(device->command.data.cmd.data.u8, &device->tenso_info, sizeof(struct zet017_tenso_info));
    struct zet017_tenso_info* tenso_info = (struct zet017_tenso_info*)(&device->command.data.cmd.data);
    for (uint32_t i = 0; i < 8; ++i) {
        tenso_info->scheme[i]        = (uint16_t)config->scheme[i];
        tenso_info->correction[i][0] = config->correction_1[i];
        tenso_info->correction[i][1] = config->correction_2[i];
    }

    device->command.command = zet017_write_tenso_config;
    device->command.result  = 0;
    device->command.state   = zet017_command_requested;
    zet017_device_wakeup(device);

    while (device->command.state != zet017_command_completed)
        zet017_cond_wait(&device->command.cond, &device->command.mutex);

    device->command.state = zet017_command_idle;
    int r = device->command.result;

    zet017_mutex_unlock(&device->command.mutex);
    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_set_tenso_config: done (number=%u)", number);
    return r;
}

ZET017_TCP_API zet017_device_start(zet017_handle handle, uint32_t number, uint32_t dac) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_start: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_start: device not found (number=%u) -> return -1", number);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_start: not connected (number=%u) -> return -2", number);
        return -2;
    }

    zet017_mutex_lock(&device->command.mutex);

    if (device->device_info.start_adc) {
        zet017_mutex_unlock(&device->command.mutex);
        SLOG(server, zet017_cat_api, zet017_log_warning,
         "zet017_device_start: already started (number=%u) -> return 0", number);
        return 0;
    }

    memcpy(&device->command.data.info, &device->device_info, sizeof(struct zet017_device_info));
    device->command.data.info.start_adc  = 1;
    device->command.data.info.start_dac  = (int16_t)dac;
    memset(&device->command.data.info.atten, 0xff, sizeof(device->command.data.info.atten));
    device->command.data.info.atten_speed = 0;

    device->command.command = zet017_start;
    device->command.result  = 0;
    device->command.state   = zet017_command_requested;
    zet017_device_wakeup(device);

    while (device->command.state != zet017_command_completed)
        zet017_cond_wait(&device->command.cond, &device->command.mutex);

    device->command.state = zet017_command_idle;
    int r = device->command.result;

    zet017_mutex_unlock(&device->command.mutex);
    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_start: done (number=%u)", number);
    return r;
}

ZET017_TCP_API zet017_device_stop(zet017_handle handle, uint32_t number) {
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_stop: entry (number=%u)", number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_stop: device not found (number=%u) -> return -1", number);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_device_stop: not connected (number=%u) -> return -2", number);
        return -2;
    }

    zet017_mutex_lock(&device->command.mutex);

    device->command.command = zet017_stop;
    device->command.result  = 0;
    device->command.state   = zet017_command_requested;
    zet017_device_wakeup(device);

    while (device->command.state != zet017_command_completed)
        zet017_cond_wait(&device->command.cond, &device->command.mutex);

    device->command.state = zet017_command_idle;

    zet017_mutex_unlock(&device->command.mutex);
    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_device_stop: done (number=%u)", number);
    return 0;
}

/* Выборка одного канала из кольцевого буфера АЦП в вольты.
 *
 * Вызывается с УЖЕ ЗАХВАЧЕННЫМ adc_data.mutex и ничего не логирует:
 * SLOG под мьютексом устройства запрещён, см. комментарий к SLOG.
 *
 * out_stride задаёт раскладку приёмника и позволяет одним и тем же проходом
 * обслуживать обе: 1 — отсчёты канала лежат подряд, work_channel — каналы
 * чередуются внутри кадра. Аргументы step и channel_size вычисляет
 * вызывающий: при чтении всех каналов они считаются один раз на вызов.
 *
 * Проверок здесь нет: и channel, и pointer, и size проверены вызывающим. */
static void zet017_adc_extract_channel_locked(
    const struct zet017_adc_data* adc, uint32_t channel,
    uint32_t pointer, uint32_t size,
    uint32_t step, uint32_t channel_size,
    float* out, uint32_t out_stride)
{
    /* Смещение канала внутри кадра: выключенные каналы в потоке
     * физически отсутствуют, поэтому учитываются только включённые. */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < channel; ++i) {
        if (adc->channel_mask & (1 << i))
            offset += adc->sample_size;
    }

    /* Читаем НАЗАД от pointer: окно [pointer - size, pointer). */
    uint32_t p = (pointer >= size) ? pointer - size : pointer + channel_size - size;
    p = p * step + offset;

    float k = adc->resolution[channel][adc->amplify_code[channel]];

    for (uint32_t i = 0; i < size; ++i, p += step) {
        if (p >= ZET017_ADC_BUFFER_SIZE)
            p -= ZET017_ADC_BUFFER_SIZE;

        float value = 0.0f;
        if (adc->sample_size == sizeof(int16_t))
            value = (float)(*(const int16_t*)(adc->buffer + p));
        else if (adc->sample_size == sizeof(int32_t))
            value = (float)(*(const int32_t*)(adc->buffer + p));

        out[i * out_stride] = value * k;
    }
}

ZET017_TCP_API zet017_channel_get_data(
    zet017_handle handle, uint32_t number, uint32_t channel,
    uint32_t pointer, float* data, uint32_t size)
{
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_channel_get_data: entry (number=%u, channel=%u)", number, channel);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_get_data: device not found (number=%u, channel=%u) -> return -1", number, channel);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_get_data: not connected (number=%u, channel=%u) -> return -3", number, channel);
        return -3;
    }

    if (!data) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_get_data: data NULL (number=%u, channel=%u) -> return -4", number, channel);
        return -4;
    }

    /* Правило блокировок: под adc_data.mutex НЕ логируем — факты
     * собираются в локальные переменные, SLOG идёт после unlock. */
    int      fail       = 0;
    int      report_ov  = 0;
    uint32_t ov_lost    = 0;
    uint32_t ov_behind  = 0;
    uint32_t ov_window  = 0;

    zet017_mutex_lock(&device->adc_data.mutex);

    for (;;) {
        if (channel >= device->adc_data.channel_quantity) { fail = -2; break; }
        if (!(device->adc_data.channel_mask & (1 << channel))) { fail = -5; break; }

        uint32_t step         = device->adc_data.sample_size * device->adc_data.work_channel;
        uint32_t channel_size = ZET017_ADC_BUFFER_SIZE / step;
        if (pointer >= channel_size || size > channel_size) { fail = -6; break; }

        /* Переполнение кольцевого буфера: цикл опроса не успевает за
         * устройством, и часть окна [pointer-size, pointer) писатель уже
         * перезаписал. Проверка выполняется только после первого оборота
         * буфера, иначе арифметика по кольцу даёт ложные срабатывания
         * на старте. */
        if (device->adc_data.frames_total >= (uint64_t)channel_size) {
            uint32_t writer = device->adc_data.pointer / step;
            uint32_t behind = (writer >= pointer)
                              ? writer - pointer
                              : writer + channel_size - pointer;
            if (behind + size > channel_size) {
                uint32_t ts = zet017_get_timestamp();
                ov_behind = behind;
                ov_lost   = behind + size - channel_size;
                ov_window = channel_size;
                /* не чаще одного сообщения в секунду на устройство */
                if (ts - device->log_overrun_ts >= 1000) {
                    device->log_overrun_ts = ts;
                    report_ov = 1;
                }
            }
        }

        zet017_adc_extract_channel_locked(&device->adc_data, channel,
                                          pointer, size, step, channel_size,
                                          data, 1);
        break;
    }

    zet017_mutex_unlock(&device->adc_data.mutex);

    if (fail != 0) {
        const char* reason = (fail == -2) ? "channel out of range"
                           : (fail == -5) ? "channel not in mask"
                                          : "pointer or size out of range";
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_get_data: %s (number=%u, channel=%u, pointer=%u, size=%u) -> return %d",
             reason, number, channel, pointer, size, fail);
        return fail;
    }

    if (report_ov) {
        SLOG(server, zet017_cat_data, zet017_log_warning,
             "adc buffer overrun (number=%u, channel=%u, behind=%u, size=%u, window=%u, lost=%u)",
             number, channel, ov_behind, size, ov_window, ov_lost);
    }

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_channel_get_data: success (number=%u, channel=%u)", number, channel);
    return 0;
}

/* Общая реализация чтения всех активных каналов за один захват мьютекса.
 *
 * Это и есть назначение функции: при поканальном чтении мьютекс отпускается
 * между вызовами, и поток приёма успевает перезаписать начало окна — тогда
 * первый канал прочитан до перезаписи, а последний уже после, то есть из
 * следующего оборота кольца. Здесь все каналы считываются из одного окна.
 *
 * planar != 0 — отсчёты каждого канала лежат подряд (строки по size);
 * planar == 0 — каналы чередуются внутри кадра, как в самом кольце.
 * Обе раскладки формирует один и тот же проход, меняется только шаг записи. */
static int zet017_channel_get_all_impl(
    zet017_handle handle, uint32_t number, uint32_t pointer,
    float* data, uint32_t size, uint32_t capacity,
    uint32_t* channels, int planar)
{
    const char* fname = planar ? "zet017_channel_get_all_data"
                               : "zet017_channel_get_all_frame_data";

    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info, "%s: entry (number=%u)", fname, number);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "%s: device not found (number=%u) -> return -1", fname, number);
        return -1;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "%s: not connected (number=%u) -> return -3", fname, number);
        return -3;
    }

    if (!data) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "%s: data NULL (number=%u) -> return -4", fname, number);
        return -4;
    }

    /* Правило блокировок: под adc_data.mutex НЕ логируем — факты
     * собираются в локальные переменные, SLOG идёт после unlock. */
    int      fail       = 0;
    int      report_ov  = 0;
    uint32_t ov_lost    = 0;
    uint32_t ov_behind  = 0;
    uint32_t ov_window  = 0;
    uint32_t work       = 0;
    uint32_t need       = 0;

    zet017_mutex_lock(&device->adc_data.mutex);

    for (;;) {
        uint32_t step         = device->adc_data.sample_size * device->adc_data.work_channel;
        uint32_t channel_size = ZET017_ADC_BUFFER_SIZE / step;

        work = device->adc_data.work_channel;

        if (pointer >= channel_size || size > channel_size) { fail = -6; break; }

        /* Переполнения произведения быть не может: work * channel_size по
         * построению равно ZET017_ADC_BUFFER_SIZE / sample_size, а size уже
         * ограничен channel_size. */
        need = work * size;
        if (capacity < need) { fail = -7; break; }

        /* Детектор переполнения кольца считается ОДИН раз на вызов: он
         * оперирует кадрами и от номера канала не зависит. Проверка
         * выполняется только после первого оборота буфера, иначе арифметика
         * по кольцу даёт ложные срабатывания на старте. */
        if (device->adc_data.frames_total >= (uint64_t)channel_size) {
            uint32_t writer = device->adc_data.pointer / step;
            uint32_t behind = (writer >= pointer)
                              ? writer - pointer
                              : writer + channel_size - pointer;
            if (behind + size > channel_size) {
                uint32_t ts = zet017_get_timestamp();
                ov_behind = behind;
                ov_lost   = behind + size - channel_size;
                ov_window = channel_size;
                /* не чаще одного сообщения в секунду на устройство */
                if (ts - device->log_overrun_ts >= 1000) {
                    device->log_overrun_ts = ts;
                    report_ov = 1;
                }
            }
        }

        /* k — порядковый номер среди ВКЛЮЧЁННЫХ каналов, он же индекс
         * строки (планарная) или позиция внутри кадра (чередующаяся). */
        uint32_t k = 0;
        for (uint32_t ch = 0; ch < device->adc_data.channel_quantity; ++ch) {
            if (!(device->adc_data.channel_mask & (1 << ch)))
                continue;

            float*   out        = planar ? data + (size_t)k * size : data + k;
            uint32_t out_stride = planar ? 1u : work;

            zet017_adc_extract_channel_locked(&device->adc_data, ch,
                                              pointer, size, step, channel_size,
                                              out, out_stride);
            ++k;
        }
        break;
    }

    zet017_mutex_unlock(&device->adc_data.mutex);

    /* Тихий указатель: channels можно не запрашивать. Заполняем и при
     * отказе -7 тоже — иначе вызывающая сторона не может определить
     * требуемый размер буфера. */
    if (channels)
        *channels = work;

    if (fail == -7) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "%s: capacity too small (number=%u, capacity=%u, need=%u) -> return -7",
             fname, number, capacity, need);
        return -7;
    }
    if (fail != 0) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "%s: pointer or size out of range (number=%u, pointer=%u, size=%u) -> return %d",
             fname, number, pointer, size, fail);
        return fail;
    }

    if (report_ov) {
        SLOG(server, zet017_cat_data, zet017_log_warning,
             "adc buffer overrun (number=%u, all channels, behind=%u, size=%u, window=%u, lost=%u)",
             number, ov_behind, size, ov_window, ov_lost);
    }

    SLOG(server, zet017_cat_api, zet017_log_info,
         "%s: success (number=%u, channels=%u, size=%u)", fname, number, work, size);
    return 0;
}

ZET017_TCP_API zet017_channel_get_all_data(
    zet017_handle handle, uint32_t number, uint32_t pointer,
    float* data, uint32_t size, uint32_t capacity, uint32_t* channels)
{
    return zet017_channel_get_all_impl(handle, number, pointer,
                                       data, size, capacity, channels, 1);
}

ZET017_TCP_API zet017_channel_get_all_frame_data(
    zet017_handle handle, uint32_t number, uint32_t pointer,
    float* data, uint32_t size, uint32_t capacity, uint32_t* channels)
{
    return zet017_channel_get_all_impl(handle, number, pointer,
                                       data, size, capacity, channels, 0);
}

ZET017_TCP_API zet017_channel_put_data(
    zet017_handle handle, uint32_t number, uint32_t channel,
    uint32_t pointer, float* data, uint32_t size)
{
    struct zet017_server* server = registry_get(handle);
    if (!server) return -1;

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_channel_put_data: entry (number=%u, channel=%u)", number, channel);

    struct zet017_device* device = zet017_get_device(server, number);
    if (!device) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_put_data: device not found (number=%u, channel=%u) -> return -1", number, channel);
        return -1;
    }

    if (channel >= ZET017_MAX_CHANNELS_DAC) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_put_data: channel out of range (number=%u, channel=%u) -> return -2", number, channel);
        return -2;
    }

    zet017_mutex_lock(&device->state_mutex);
    uint16_t is_connected = device->state.is_connected;
    zet017_mutex_unlock(&device->state_mutex);
    if (!is_connected) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_put_data: not connected (number=%u, channel=%u) -> return -3", number, channel);
        return -3;
    }

    if (!data) {
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_put_data: data NULL (number=%u, channel=%u) -> return -4", number, channel);
        return -4;
    }

    /* Правило блокировок: логируем после снятия dac_data.mutex */
    int fail = 0;

    zet017_mutex_lock(&device->dac_data.mutex);

    for (;;) {
        if (!(device->dac_data.channel_mask & (1 << channel))) { fail = -5; break; }

        uint32_t step         = device->dac_data.sample_size * device->dac_data.channel_quantity;
        uint32_t channel_size = ZET017_DAC_BUFFER_SIZE / step;
        if (pointer >= channel_size || size > channel_size) { fail = -6; break; }

        uint32_t offset = 0;
        for (uint32_t i = 0; i < channel; ++i) {
            if (device->dac_data.channel_mask & (1 << i))
                offset += device->dac_data.sample_size;
        }

        uint32_t p = (pointer >= size) ? pointer - size : pointer + channel_size - size;
        p = p * step + offset;

        for (uint32_t i = 0; i < size; ++i, p += step) {
            if (p >= ZET017_DAC_BUFFER_SIZE)
                p -= ZET017_DAC_BUFFER_SIZE;

            if (device->dac_data.sample_size == sizeof(int16_t))
                *(int16_t*)(device->dac_data.buffer + p) = (int16_t)(data[i] / device->dac_data.resolution[channel]);
            else if (device->dac_data.sample_size == sizeof(int32_t))
                *(int32_t*)(device->dac_data.buffer + p) = (int32_t)(data[i] / device->dac_data.resolution[channel]);
        }
        break;
    }

    zet017_mutex_unlock(&device->dac_data.mutex);

    if (fail != 0) {
        const char* reason = (fail == -5) ? "channel not in mask"
                                          : "pointer or size out of range";
        SLOG(server, zet017_cat_api, zet017_log_warning,
             "zet017_channel_put_data: %s (number=%u, channel=%u, pointer=%u, size=%u) -> return %d",
             reason, number, channel, pointer, size, fail);
        return fail;
    }

    SLOG(server, zet017_cat_api, zet017_log_info,
         "zet017_channel_put_data: success (number=%u, channel=%u)", number, channel);
    return 0;
}
