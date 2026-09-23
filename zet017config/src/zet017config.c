/* zet017config — отдельный модуль чтения и записи XML-конфигурации ZET 017.
 *
 * Единственная задача: перенести значения между XML-файлом и структурами
 * zet017_cfg_config / zet017_cfg_tenso_config в обе стороны. Никакого
 * сетевого взаимодействия с устройством здесь нет — этим занимается основная
 * библиотека zet017tcp. Именно поэтому вся зависимость от libxml2
 * изолирована в этой DLL, а основная библиотека остаётся без зависимостей.
 *
 * Обе операции построены на «тихом указателе»: обязательный указатель,
 * равный NULL, — это ошибка без побочных эффектов, а необязательный,
 * равный NULL, исключает соответствующую часть работы. Симметрично устроены
 * и данные: при чтении отсутствующий элемент оставляет поле структуры
 * неизменным, при записи отсутствующий элемент файла не создаётся, а всё,
 * чем модуль не управляет, переносится в новый файл без изменений.
 *
 * Формат XML (version="1.2"):
 *   <Config version="1.2">
 *     <Device serial="12345">
 *       <Channel>...</Channel>          маска активных каналов АЦП
 *       <HCPChannel>...</HCPChannel>    маска ICP
 *       <ModaADC>...</ModaADC>          код режима частоты АЦП
 *       <Freq>...</Freq>                та же частота АЦП в Гц (только запись)
 *       <RateDAC>...</RateDAC>          код частоты ЦАП
 *       <KodAmplify>a,b,c,...</KodAmplify>  коды усиления по каналам
 *       <BuiltinGenActive>0|1</BuiltinGenActive>
 *       <BuiltinGenSineActive>0|1</BuiltinGenSineActive>
 *       <BuiltinGenSineFreq>...</BuiltinGenSineFreq>
 *       <BuiltinGenSineAmpl>...</BuiltinGenSineAmpl>
 *       <BuiltinGenSineBias>...</BuiltinGenSineBias>
 *       <Channels>
 *         <Channel id="0"><Tenso>..</Tenso><Pot1>..</Pot1><Pot2>..</Pot2></Channel>
 *         ...
 *       </Channels>
 *     </Device>
 *   </Config>
 *
 * Это ПОДМНОЖЕСТВО. Настоящий devices.cfg содержит ещё около двух десятков
 * элементов на устройство (Rate, Amplitude, DigitalResolChanADC, Atten,
 * DigitalInput/Output, sizeInterrupt, ...) и по полтора десятка на канал
 * (Sense, units, Amplify, TensoIcp, ...), а KodAmplify в нём — список из 32
 * позиций. Всё это приходит от устройства в служебной структуре, которую
 * zet017tcp не объявляет в публичном API, поэтому сформировать такой файл
 * целиком модуль не может: чтение извлекает перечисленное выше, запись
 * обновляет его же, остальное содержимое файла остаётся неизменным.
 */

#include "zet017config.h"

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <libxml/parser.h>

/* Потокобезопасный strtok: strtok_r (POSIX) отсутствует в MSVC,
 * где его аналог называется strtok_s с той же сигнатурой. */
#if defined(_MSC_VER)
#  define zet017_cfg_strtok(str, delim, ctx) strtok_s((str), (delim), (ctx))
#else
#  define zet017_cfg_strtok(str, delim, ctx) strtok_r((str), (delim), (ctx))
#endif

/* Разделитель пути и имя файла конфигурации по умолчанию. */
#if defined(_WIN32)
#  define ZET017_CFG_PATH_SEP "\\"
#else
#  define ZET017_CFG_PATH_SEP "/"
#endif
#define ZET017_CFG_DEFAULT_FILE "devices.cfg"

/* Максимальная длина пути к файлу конфигурации (включая '\0').
 * 260 — MAX_PATH, та же граница, что у ZET017_LOG_PATH_MAX в zet017tcp. */
#define ZET017_CFG_PATH_MAX 260

/* Длина буфера для списка через запятую (<KodAmplify>): в реальном файле
 * 32 позиции, запас взят на случай более длинных списков в будущих прошивках. */
#define ZET017_CFG_CSV_MAX 1024

/* Разбор дробного числа, не зависящий от локали.
 *
 * В файле разделитель дробной части — всегда точка, а strtod смотрит на
 * LC_NUMERIC: под русской локалью он читает "1.2" как 1, и проверка версии
 * отклоняет корректный документ. Подставляется разделитель текущей локали,
 * после чего выполняется обычный разбор.
 *
 * Локаль процесса модулю не принадлежит — со статическим рантаймом (/MT) у DLL
 * своя копия CRT и локаль остаётся "C", а с динамическим (/MD) она общая с
 * LabView, — поэтому setlocale здесь не вызывается ни при каких условиях. */
static double zet017_cfg_strtod(const char* s)
{
    if (s == NULL)
        return 0.0;

    char point = '.';
    struct lconv* lc = localeconv();
    if (lc != NULL && lc->decimal_point != NULL && lc->decimal_point[0] != '\0')
        point = lc->decimal_point[0];

    char buf[64];
    size_t n = strlen(s);
    if (point == '.' || n >= sizeof(buf))
        return strtod(s, NULL);

    for (size_t i = 0; i <= n; ++i)
        buf[i] = (s[i] == '.') ? point : s[i];

    return strtod(buf, NULL);
}

/* Найти <Device serial="..."> с нужным серийным номером и одновременно
 * проверить заголовок документа. Общая часть чтения и записи.
 *
 * Возвращает 0 и устройство в *device_node, либо -3/-4/-5/-6 — те же коды,
 * что и обе экспортируемые функции. */
static int zet017_config_find_device(xmlDocPtr doc, zet017_uint32 serial, xmlNodePtr* device_node)
{
    *device_node = NULL;

    xmlNodePtr config_node = xmlDocGetRootElement(doc);
    if (config_node == NULL)
        return -3;

    if (xmlStrcmp(config_node->name, (const xmlChar*)"Config") != 0)
        return -3;

    xmlChar* str_version = xmlGetProp(config_node, (const xmlChar*)"version");
    if (str_version == NULL)
        return -4;

    double version = zet017_cfg_strtod((const char*)str_version);
    xmlFree(str_version);
    if (version != 1.2)
        return -5;

    for (xmlNodePtr node = config_node->children; node != NULL; node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrcmp(node->name, (const xmlChar*)"Device") != 0)
            continue;

        xmlChar* str_serial = xmlGetProp(node, (const xmlChar*)"serial");
        if (str_serial == NULL)
            return -6;

        uint32_t dev_serial = (uint32_t)strtoul((const char*)str_serial, NULL, 10);
        xmlFree(str_serial);

        if (dev_serial == serial) {
            *device_node = node;
            return 0;
        }
    }

    return -6;
}

/* Разобрать уже распарсенный XML-документ и заполнить структуры. */
static int zet017_config_load_doc(
    xmlDocPtr doc, zet017_uint32 serial,
    struct zet017_cfg_config* config, struct zet017_cfg_tenso_config* tenso_config)
{
    if (!doc || !config)
        return -1;

    xmlNodePtr device_node = NULL;
    int r = zet017_config_find_device(doc, serial, &device_node);
    if (r != 0)
        return r;

    for (xmlNodePtr child = device_node->children; child != NULL; child = child->next) {
        if (child->type != XML_ELEMENT_NODE)
            continue;

        if (xmlStrcmp(child->name, (const xmlChar*)"Channel") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->mask_channel_adc = (uint32_t)strtoul((const char*)s, NULL, 10);
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"HCPChannel") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->mask_icp = (uint32_t)strtoul((const char*)s, NULL, 10);
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"ModaADC") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->moda_adc = (uint16_t)strtoul((const char*)s, NULL, 10);
                config->sample_rate_adc = 0;  /* код режима имеет приоритет над частотой */
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"RateDAC") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->rate_dac = (uint16_t)strtoul((const char*)s, NULL, 10);
                config->sample_rate_dac = 0;
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"KodAmplify") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                char* saveptr = NULL;
                char* token = zet017_cfg_strtok((char*)s, ",", &saveptr);
                uint16_t i = 0;
                while (token != NULL) {
                    config->gain_code[i] = (uint16_t)strtoul(token, NULL, 10);
                    config->gain[i] = 0;  /* сырой код имеет приоритет над gain */
                    if (++i >= sizeof(config->gain) / sizeof(config->gain[0]))
                        break;
                    token = zet017_cfg_strtok(NULL, ",", &saveptr);
                }
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"BuiltinGenActive") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                if (strtoul((const char*)s, NULL, 10))
                    config->builtin_dac_state |= 0x1;
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"BuiltinGenSineActive") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                if (strtoul((const char*)s, NULL, 10))
                    config->builtin_dac_state |= 0x2;
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"BuiltinGenSineFreq") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->builtin_dac_sine_freq = zet017_cfg_strtod((const char*)s);
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"BuiltinGenSineAmpl") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->builtin_dac_sine_ampl = zet017_cfg_strtod((const char*)s);
                xmlFree(s);
            }
        }
        else if (xmlStrcmp(child->name, (const xmlChar*)"BuiltinGenSineBias") == 0) {
            if (child->children != NULL) {
                xmlChar* s = xmlNodeGetContent(child->children);
                config->builtin_dac_sine_offset = zet017_cfg_strtod((const char*)s);
                xmlFree(s);
            }
        }

        if (xmlStrcmp(child->name, (const xmlChar*)"Channels") == 0 && tenso_config != NULL) {
            for (xmlNodePtr ch = child->children; ch != NULL; ch = ch->next) {
                if (ch->type != XML_ELEMENT_NODE)
                    continue;
                if (xmlStrcmp(ch->name, (const xmlChar*)"Channel") != 0)
                    continue;

                xmlChar* str_id = xmlGetProp(ch, (const xmlChar*)"id");
                if (str_id == NULL)
                    continue;
                uint32_t i = (uint32_t)strtoul((const char*)str_id, NULL, 10);
                xmlFree(str_id);
                if (i >= 8)
                    continue;

                for (xmlNodePtr cc = ch->children; cc != NULL; cc = cc->next) {
                    if (cc->type != XML_ELEMENT_NODE)
                        continue;

                    if (xmlStrcmp(cc->name, (const xmlChar*)"Tenso") == 0) {
                        if (cc->children != NULL) {
                            xmlChar* s = xmlNodeGetContent(cc->children);
                            tenso_config->scheme[i] = (enum zet017_cfg_scheme)strtol((const char*)s, NULL, 10);
                            xmlFree(s);
                        }
                    }
                    else if (xmlStrcmp(cc->name, (const xmlChar*)"Pot1") == 0) {
                        if (cc->children != NULL) {
                            xmlChar* s = xmlNodeGetContent(cc->children);
                            tenso_config->correction_1[i] = (uint8_t)strtoul((const char*)s, NULL, 10);
                            xmlFree(s);
                        }
                    }
                    else if (xmlStrcmp(cc->name, (const xmlChar*)"Pot2") == 0) {
                        if (cc->children != NULL) {
                            xmlChar* s = xmlNodeGetContent(cc->children);
                            tenso_config->correction_2[i] = (uint8_t)strtoul((const char*)s, NULL, 10);
                            xmlFree(s);
                        }
                    }
                }
            }
        }
    }

    return 0;
}

ZET017_CONFIG_API zet017_config_load_file(
    const char* file_name, zet017_uint32 serial,
    struct zet017_cfg_config* config, struct zet017_cfg_tenso_config* tenso_config)
{
    if (!file_name || !config)
        return -1;

    xmlDocPtr doc = xmlParseFile(file_name);
    if (doc == NULL)
        return -2;

    int r = zet017_config_load_doc(doc, serial, config, tenso_config);

    xmlFreeDoc(doc);

    /* xmlCleanupParser() намеренно НЕ вызывается: в DLL с многократными
     * вызовами и/или несколькими потоками он глобально разрушает состояние
     * libxml2 и приводит к аварийным ситуациям. Освобождение глобальных
     * ресурсов libxml2 остаётся на усмотрение хост-процесса. */

    return r;
}

/* ==========================================================================
 *  Запись
 * ========================================================================== */

/* --------------------------------------------------------------------------
 * Таблицы перевода «величина ↔ сырой код».
 *
 * ДУБЛИРУЮТСЯ из zet017tcp/src/zet017tcp.c (функции zet017_get_mode_adc,
 * zet017_get_sample_rate_adc, zet017_get_rate_dac, zet017_get_amplify_code).
 * Запись обязана разрешать приоритет полей ровно так же, как это делает
 * zet017_device_set_config, иначе записанное в файл разойдётся с тем, что
 * передаётся устройству. Модуль намеренно независим от zet017tcp и вызвать
 * оригинал не может, поэтому при изменении таблиц там их нужно изменить
 * и здесь —
 * так же, как продублированные структуры в zet017config.h.
 * -------------------------------------------------------------------------- */
static uint16_t zet017_cfg_get_mode_adc(uint32_t sample_rate_adc)
{
    switch (sample_rate_adc) {
    case 50000: return 1;
    case 25000: return 2;
    case 5000:  return 3;
    case 2500:  return 4;
    default:    break;
    }
    return 0;
}

static uint32_t zet017_cfg_get_sample_rate_adc(uint16_t mode_adc)
{
    switch (mode_adc) {
    case 1: return 50000;
    case 3: return 5000;
    case 4: return 2500;
    default: break;
    }
    return 25000;
}

static uint16_t zet017_cfg_get_rate_dac(uint32_t sample_rate_dac)
{
    return sample_rate_dac ? (uint16_t)(80000000u / sample_rate_dac) : 0;
}

static uint16_t zet017_cfg_get_amplify_code(uint32_t gain)
{
    switch (gain) {
    case 1:   return 0;
    case 10:  return 1;
    case 100: return 2;
    default:  break;
    }
    return 0;
}

/* Форматирование double с ТОЧКОЙ в качестве десятичного разделителя.
 * snprintf учитывает LC_NUMERIC: под русской локалью он сформировал бы
 * "1,5", и такой файл перестал бы читаться — strtod ожидает точку. */
static void zet017_cfg_fmt_double(char* buf, size_t size, double value)
{
    snprintf(buf, size, "%.10g", value);
    for (char* p = buf; *p != '\0'; ++p)
        if (*p == ',')
            *p = '.';
}

/* Собрать путь к файлу конфигурации. Возвращает 0 или -8, если не влезает. */
static int zet017_cfg_build_path(char* buf, size_t size, const char* dir_name, const char* file_name)
{
    if (!file_name || file_name[0] == '\0')
        file_name = ZET017_CFG_DEFAULT_FILE;

    /* Разделитель не нужен, если каталог уже им заканчивается. Проверяются
     * оба варианта: на Windows в путях встречаются и '\', и '/'. */
    size_t dir_len = strlen(dir_name);
    const char* sep = ZET017_CFG_PATH_SEP;
    if (dir_len > 0 && (dir_name[dir_len - 1] == '\\' || dir_name[dir_len - 1] == '/'))
        sep = "";

    int n = snprintf(buf, size, "%s%s%s", dir_name, sep, file_name);
    if (n < 0 || (size_t)n >= size)
        return -8;

    return 0;
}

/* Первый дочерний элемент с указанным именем (или NULL). */
static xmlNodePtr zet017_cfg_find_child(xmlNodePtr parent, const char* name)
{
    if (parent == NULL)
        return NULL;

    for (xmlNodePtr node = parent->children; node != NULL; node = node->next) {
        if (node->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrcmp(node->name, (const xmlChar*)name) == 0)
            return node;
    }
    return NULL;
}

/* Записать текст в дочерний элемент.
 *
 * Если элемента нет — он добавляется перед закрывающим тегом родителя с тем
 * же отступом, что и у соседей, чтобы не нарушать форматирование файла.
 * Все значения здесь числовые, спецсимволов XML в них не бывает, поэтому
 * экранирование не требуется. */
static void zet017_cfg_set_child_text(xmlNodePtr parent, const char* name, const char* text)
{
    if (parent == NULL)
        return;

    xmlNodePtr node = zet017_cfg_find_child(parent, name);
    if (node != NULL) {
        xmlNodeSetContent(node, (const xmlChar*)text);
        return;
    }

    /* Отступ берём у последнего элемента-соседа: текстовый узел перед ним. */
    xmlNodePtr last_element = NULL;
    for (xmlNodePtr n = parent->children; n != NULL; n = n->next)
        if (n->type == XML_ELEMENT_NODE)
            last_element = n;

    xmlNodePtr indent = NULL;
    if (last_element != NULL && last_element->prev != NULL &&
        last_element->prev->type == XML_TEXT_NODE) {
        xmlChar* s = xmlNodeGetContent(last_element->prev);
        if (s != NULL) {
            indent = xmlNewText(s);
            xmlFree(s);
        }
    }

    xmlNodePtr created = xmlNewNode(NULL, (const xmlChar*)name);
    if (created == NULL) {
        if (indent != NULL)
            xmlFreeNode(indent);
        return;
    }
    xmlNodeSetContent(created, (const xmlChar*)text);

    /* Последний потомок родителя — обычно отступ перед закрывающим тегом;
     * элемент вставляется ПЕРЕД ним, иначе он окажется за ним.
     *
     * Сначала элемент, только потом отступ: xmlAddPrevSibling объединяет
     * соседние текстовые узлы, и отступ, добавленный перед хвостовым
     * пробельным узлом, слился бы с ним. Перед элементом объединять
     * не с чем. */
    xmlNodePtr tail = parent->last;
    if (tail != NULL && tail->type == XML_TEXT_NODE) {
        xmlAddPrevSibling(tail, created);
        if (indent != NULL)
            xmlAddPrevSibling(created, indent);
    }
    else {
        if (indent != NULL)
            xmlAddChild(parent, indent);
        xmlAddChild(parent, created);
    }
}

/* Заменить первые count значений в списке через запятую, сохранив
 * оставшиеся.
 *
 * Нужно для <KodAmplify>: в реальном devices.cfg это 32 позиции, а модуль
 * формирует только первые восемь — остальные принадлежат устройству и
 * должны вернуться в файл без изменений.
 *
 * Возвращает 0 или -7, если собранная строка не помещается в буфер
 * (лучше отказать, чем записать усечённый список). */
static int zet017_cfg_patch_csv_prefix(
    xmlNodePtr parent, const char* name, const uint16_t* values, unsigned count)
{
    char out[ZET017_CFG_CSV_MAX];
    size_t used = 0;

    for (unsigned i = 0; i < count; ++i) {
        int n = snprintf(out + used, sizeof(out) - used, i ? ",%u" : "%u", (unsigned)values[i]);
        if (n < 0 || (size_t)n >= sizeof(out) - used)
            return -7;
        used += (size_t)n;
    }

    /* Хвост исходного списка дописываем как есть. */
    xmlNodePtr node = zet017_cfg_find_child(parent, name);
    if (node != NULL && node->children != NULL) {
        xmlChar* s = xmlNodeGetContent(node->children);
        if (s != NULL) {
            char* saveptr = NULL;
            char* token = zet017_cfg_strtok((char*)s, ",", &saveptr);
            unsigned pos = 0;
            while (token != NULL) {
                if (pos >= count) {
                    int n = snprintf(out + used, sizeof(out) - used, ",%s", token);
                    if (n < 0 || (size_t)n >= sizeof(out) - used) {
                        xmlFree(s);
                        return -7;
                    }
                    used += (size_t)n;
                }
                ++pos;
                token = zet017_cfg_strtok(NULL, ",", &saveptr);
            }
            xmlFree(s);
        }
    }

    zet017_cfg_set_child_text(parent, name, out);
    return 0;
}

/* Обновить в найденном <Device> те элементы, которыми владеет модуль. */
static int zet017_config_save_device(
    xmlNodePtr device_node,
    const struct zet017_cfg_config* config, const struct zet017_cfg_tenso_config* tenso_config)
{
    char buf[64];

    snprintf(buf, sizeof(buf), "%u", (unsigned)config->mask_channel_adc);
    zet017_cfg_set_child_text(device_node, "Channel", buf);

    snprintf(buf, sizeof(buf), "%u", (unsigned)config->mask_icp);
    zet017_cfg_set_child_text(device_node, "HCPChannel", buf);

    /* Правило то же, что в zet017_device_set_config: заданная частота имеет
     * приоритет над сырым кодом. <Freq> — та же величина в герцах; в файле
     * она расположена рядом с <ModaADC>, и сохранение прежнего значения
     * сделало бы файл внутренне противоречивым. <Rate> не изменяется:
     * аналога в структурах нет и вычислить его не из чего. */
    uint16_t mode_adc = config->sample_rate_adc
        ? zet017_cfg_get_mode_adc(config->sample_rate_adc)
        : config->moda_adc;
    uint32_t sample_rate_adc = config->sample_rate_adc
        ? config->sample_rate_adc
        : zet017_cfg_get_sample_rate_adc(config->moda_adc);

    snprintf(buf, sizeof(buf), "%u", (unsigned)mode_adc);
    zet017_cfg_set_child_text(device_node, "ModaADC", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)sample_rate_adc);
    zet017_cfg_set_child_text(device_node, "Freq", buf);

    uint16_t rate_dac = config->sample_rate_dac
        ? zet017_cfg_get_rate_dac(config->sample_rate_dac)
        : config->rate_dac;
    snprintf(buf, sizeof(buf), "%u", (unsigned)rate_dac);
    zet017_cfg_set_child_text(device_node, "RateDAC", buf);

    uint16_t amplify_code[8];
    for (unsigned i = 0; i < 8; ++i) {
        amplify_code[i] = config->gain[i]
            ? zet017_cfg_get_amplify_code(config->gain[i])
            : config->gain_code[i];
    }
    int r = zet017_cfg_patch_csv_prefix(device_node, "KodAmplify", amplify_code, 8);
    if (r != 0)
        return r;

    zet017_cfg_set_child_text(device_node, "BuiltinGenActive",
                              (config->builtin_dac_state & 0x1) ? "1" : "0");
    zet017_cfg_set_child_text(device_node, "BuiltinGenSineActive",
                              (config->builtin_dac_state & 0x2) ? "1" : "0");

    zet017_cfg_fmt_double(buf, sizeof(buf), config->builtin_dac_sine_freq);
    zet017_cfg_set_child_text(device_node, "BuiltinGenSineFreq", buf);
    zet017_cfg_fmt_double(buf, sizeof(buf), config->builtin_dac_sine_ampl);
    zet017_cfg_set_child_text(device_node, "BuiltinGenSineAmpl", buf);
    zet017_cfg_fmt_double(buf, sizeof(buf), config->builtin_dac_sine_offset);
    zet017_cfg_set_child_text(device_node, "BuiltinGenSineBias", buf);

    /* Тихий указатель: без tenso_config секция <Channels> не открывается. */
    if (tenso_config == NULL)
        return 0;

    xmlNodePtr channels = zet017_cfg_find_child(device_node, "Channels");
    if (channels == NULL)
        return 0;

    for (xmlNodePtr ch = channels->children; ch != NULL; ch = ch->next) {
        if (ch->type != XML_ELEMENT_NODE)
            continue;
        if (xmlStrcmp(ch->name, (const xmlChar*)"Channel") != 0)
            continue;

        xmlChar* str_id = xmlGetProp(ch, (const xmlChar*)"id");
        if (str_id == NULL)
            continue;
        uint32_t i = (uint32_t)strtoul((const char*)str_id, NULL, 10);
        xmlFree(str_id);
        if (i >= 8)
            continue;  /* канал 8 — встроенный генератор, он не наш */

        /* UNKNOWN (-1) означает «схема не задана» и допустимым значением для
         * ZETLAB не является — такой элемент остаётся без изменений. */
        if (tenso_config->scheme[i] != ZET017_CFG_SCHEME_UNKNOWN) {
            snprintf(buf, sizeof(buf), "%d", (int)tenso_config->scheme[i]);
            zet017_cfg_set_child_text(ch, "Tenso", buf);
        }

        snprintf(buf, sizeof(buf), "%u", (unsigned)tenso_config->correction_1[i]);
        zet017_cfg_set_child_text(ch, "Pot1", buf);
        snprintf(buf, sizeof(buf), "%u", (unsigned)tenso_config->correction_2[i]);
        zet017_cfg_set_child_text(ch, "Pot2", buf);
    }

    return 0;
}

/* Использует ли файл переводы строки в стиле Windows (CRLF).
 *
 * libxml2 по требованию стандарта XML сворачивает CRLF в одиночный LF ещё на
 * разборе, а ZETLAB пишет devices.cfg в стиле Windows. Без восстановления
 * исходного варианта сохранение переписывало бы КАЖДУЮ строку файла.
 * Анализируется первый перевод строки — он находится в объявлении XML,
 * то есть заведомо в начале файла.
 *
 * Одновременно это проверка, что файл открывается: -1 означает отсутствие
 * исходного файла, и тогда libxml2 не вызывается — иначе на каждую такую
 * попытку она писала бы в stderr предупреждение о ненайденной сущности. */
static int zet017_cfg_file_has_crlf(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f == NULL)
        return -1;

    char head[512];
    size_t n = fread(head, 1, sizeof(head), f);
    fclose(f);

    for (size_t i = 0; i + 1 < n; ++i) {
        if (head[i] == '\n')
            return 0;
        if (head[i] == '\r' && head[i + 1] == '\n')
            return 1;
    }
    return 0;
}

/* Записать готовый XML в файл, восстановив оформление исходного:
 *   - LF разворачивается обратно в CRLF, если исходный файл был в стиле
 *     Windows;
 *   - пустой тег закрывается как "<AFCH />", а не "<AFCH/>" (libxml2 пробел
 *     не хранит, а ZETLAB его ставит).
 *
 * Оба преобразования затрагивают только оформление, сам XML не меняется.
 * Без них каждое сохранение переписывало бы десятки строк, которые не
 * изменялись, и сравнение версий файла потеряло бы смысл.
 *
 * Пробел добавляется только внутри тега-элемента и вне кавычек, поэтому
 * последовательность "/>" в тексте, в значении атрибута, в комментарии или
 * в CDATA не изменяется. */
static int zet017_cfg_write_file(const char* path, const xmlChar* data, int size, int crlf)
{
    FILE* f = fopen(path, "wb");
    if (f == NULL)
        return -1;

    int in_element = 0;  /* внутри тега <имя ...>, а не <?...?>, <!--...--> */
    int quote = 0;       /* открывающая кавычка значения атрибута */
    int ok = 1;

    for (int i = 0; i < size; ++i) {
        char c = (char)data[i];

        if (in_element) {
            if (quote != 0) {
                if (c == quote)
                    quote = 0;
            }
            else if (c == '"' || c == '\'') {
                quote = c;
            }
            else if (c == '>') {
                in_element = 0;
            }
            else if (c == '/' && i + 1 < size && data[i + 1] == '>' &&
                     i > 0 && data[i - 1] != ' ') {
                if (fputc(' ', f) == EOF) { ok = 0; break; }
            }
        }
        else if (c == '<' && i + 1 < size) {
            char next = (char)data[i + 1];
            in_element = (next != '?' && next != '!');
        }

        if (crlf && c == '\n' && fputc('\r', f) == EOF) { ok = 0; break; }
        if (fputc(c, f) == EOF) { ok = 0; break; }
    }

    if (fclose(f) != 0)
        ok = 0;

    return ok ? 0 : -1;
}

/* Заменить исходный файл временным.
 * POSIX rename() перезаписывает цель атомарно; на Windows это делает
 * MoveFileEx с MOVEFILE_REPLACE_EXISTING (обычный rename() там отказывает,
 * если файл уже существует). */
static int zet017_cfg_replace_file(const char* from, const char* to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

ZET017_CONFIG_API zet017_config_save_file(
    const char* dir_name, const char* file_name, zet017_uint32 serial,
    const struct zet017_cfg_config* config, const struct zet017_cfg_tenso_config* tenso_config)
{
    if (!dir_name || dir_name[0] == '\0' || !config)
        return -1;

    char path[ZET017_CFG_PATH_MAX];
    if (zet017_cfg_build_path(path, sizeof(path), dir_name, file_name) != 0)
        return -8;

    /* Файл-донор обязателен: иного источника для элементов, которыми модуль
     * не управляет, нет. Стиль переводов строки определяется ДО разбора —
     * парсер их нормализует. */
    int crlf = zet017_cfg_file_has_crlf(path);
    if (crlf < 0)
        return -2;

    xmlDocPtr doc = xmlParseFile(path);
    if (doc == NULL)
        return -2;

    xmlNodePtr device_node = NULL;
    int r = zet017_config_find_device(doc, serial, &device_node);
    if (r == 0)
        r = zet017_config_save_device(device_node, config, tenso_config);
    if (r != 0) {
        xmlFreeDoc(doc);
        return r;
    }

    /* Запись идёт во временный файл рядом, которым затем заменяется исходный:
     * донор прочитан из этого же файла, и прямая запись при сбое уничтожила
     * бы единственную копию конфигурации. */
    char tmp_path[ZET017_CFG_PATH_MAX + 8];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        xmlFreeDoc(doc);
        return -8;
    }

    /* Выгрузка в память идёт без переформатирования: отступы, порядок
     * элементов и незнакомые модулю секции переносятся так, как их разобрал
     * парсер.
     *
     * Кодировка указывается ЯВНО. У ZETLAB объявление XML идёт без неё, и тогда
     * libxml2 при выгрузке считает выходной поток небезопасным для не-ASCII и
     * заменяет кириллицу в атрибутах ссылками на символы: name="ZET 058 №..."
     * превращается в "ZET 058 &#x2116;...". Формально это тот же документ,
     * но он становится нечитаемым для человека, а упрощённый разборщик на
     * стороне ZETLAB может его не обработать. С encoding="UTF-8" кириллица
     * остаётся как есть — ценой одной изменившейся строки объявления. */
    xmlChar* out = NULL;
    int out_size = 0;
    xmlDocDumpMemoryEnc(doc, &out, &out_size, "UTF-8");

    xmlFreeDoc(doc);

    /* xmlCleanupParser() намеренно НЕ вызывается — см. комментарий в
     * zet017_config_load_file. */

    if (out == NULL)
        return -7;

    int written = zet017_cfg_write_file(tmp_path, out, out_size, crlf);
    xmlFree(out);

    if (written != 0) {
        remove(tmp_path);
        return -7;
    }

    if (zet017_cfg_replace_file(tmp_path, path) != 0) {
        remove(tmp_path);
        return -7;
    }

    return 0;
}
