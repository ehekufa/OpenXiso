#define _POSIX_C_SOURCE 200809L

/*
 * openxiso_core.c — ядро OpenXiso на чистом C11.
 *
 * Здесь находится «архитектурная петля данных»: состояние загрузчика,
 * флаги готовности, черновик конфигурации, список созданных машин и разбор
 * файла образа. Файл не зависит от JNI и Android, поэтому его можно собрать
 * обычным gcc на хосте (см. tests/Makefile) и покрыть модульными тестами.
 * Все сообщения и диагностика — на русском языке в кодировке UTF-8.
 */

#include "include/openxiso_core.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct OpenXisoBootMessage {
    OpenXisoBootStage stage;
    const char* text;
} OpenXisoBootMessage;

typedef struct OpenXisoConfiguration {
    long long id;
    char os_name[OPENXISO_NAME_CAPACITY];
    char image_uri[OPENXISO_URI_CAPACITY];
    char image_name[OPENXISO_NAME_CAPACITY];
    char image_info[OPENXISO_INFO_CAPACITY];
} OpenXisoConfiguration;

struct OpenXisoEngine {
    /* Флаги и стадии инициализации. */
    int boot_complete;
    int boot_step_index;
    OpenXisoBootStage boot_stage;

    /* Черновик формы. */
    char os_name[OPENXISO_NAME_CAPACITY];
    char image_uri[OPENXISO_URI_CAPACITY];
    char image_name[OPENXISO_NAME_CAPACITY];
    char image_info[OPENXISO_INFO_CAPACITY];

    /* Список конфигураций и диагностика. */
    OpenXisoConfiguration* items;
    size_t count;
    size_t capacity;
    long long next_id;
    OpenXisoStatus last_status;
};

/* Низкоуровневые строки загрузчика, обновляются каждые 2,5 с. */
static const OpenXisoBootMessage kBootMessages[OPENXISO_BOOT_MESSAGE_COUNT] = {
    { OPENXISO_STAGE_ARCHITECTURE, "Загрузка архитектурных библиотек..." },
    { OPENXISO_STAGE_JIT_CORE,     "Инициализация JIT-ядра x86_64..." },
    { OPENXISO_STAGE_MEMORY,       "Картирование страничной памяти MMU..." },
    { OPENXISO_STAGE_STORAGE,      "Монтирование виртуальных накопителей..." },
    { OPENXISO_STAGE_DEVICES,      "Проверка устройств PCI и USB..." }
};

static const char* const kStatusMessages[] = {
    "Операция выполнена",
    "Некорректный аргумент нативного вызова",
    "Не удалось определить имя выбранного файла",
    "Поддерживаются только файлы с расширением .iso или .img",
    "Заполните название системы и выберите файл образа",
    "Недостаточно памяти для новой конфигурации",
    "Не удалось прочитать файл образа"
};

/* ---------------- Внутренние помощники ---------------- */

/** Безопасная копия строки с обязательным нулевым байтом на конце. */
static void copy_text(char* destination, size_t capacity, const char* source) {
    if (destination == NULL || capacity == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }
    snprintf(destination, capacity, "%s", source);
}

/** Копирует название системы, отсекая ведущие и ведомые пробелы. */
static void copy_trimmed(char* destination, size_t capacity, const char* source) {
    copy_text(destination, capacity, source);
    if (destination[0] == '\0') {
        return;
    }
    size_t length = strlen(destination);
    size_t start = 0;
    while (start < length && isspace((unsigned char)destination[start])) {
        ++start;
    }
    while (length > start && isspace((unsigned char)destination[length - 1])) {
        --length;
    }
    if (start > 0) {
        memmove(destination, destination + start, length - start);
    }
    destination[length - start] = '\0';
}

/** Проверка суффикса без учёта регистра. */
static int ends_with_ignore_case(const char* value, const char* suffix) {
    if (value == NULL || suffix == NULL) {
        return 0;
    }
    const size_t value_length = strlen(value);
    const size_t suffix_length = strlen(suffix);
    if (suffix_length > value_length) {
        return 0;
    }
    const char* tail = value + value_length - suffix_length;
    for (size_t i = 0; i < suffix_length; ++i) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i])) {
            return 0;
        }
    }
    return 1;
}

/** Строка считается заполненной, если содержит хотя бы один непробельный символ. */
static int has_content(const char* value) {
    if (value == NULL) {
        return 0;
    }
    for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; ++p) {
        if (!isspace(*p)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t read_le32(const unsigned char* bytes) {
    return (uint32_t)bytes[0]
           | ((uint32_t)bytes[1] << 8)
           | ((uint32_t)bytes[2] << 16)
           | ((uint32_t)bytes[3] << 24);
}

static long long megabytes(long long bytes) {
    if (bytes <= 0) {
        return 0;
    }
    return (bytes + 1024LL * 1024LL - 1LL) / (1024LL * 1024LL);
}

/* ---------------- Жизненный цикл движка ---------------- */

OpenXisoEngine* openxiso_engine_create(void) {
    OpenXisoEngine* engine = (OpenXisoEngine*)calloc(1, sizeof(OpenXisoEngine));
    if (engine == NULL) {
        return NULL;
    }
    engine->boot_stage = OPENXISO_STAGE_ARCHITECTURE;
    engine->next_id = 1;
    engine->last_status = OPENXISO_OK;
    return engine;
}

void openxiso_engine_destroy(OpenXisoEngine* engine) {
    if (engine == NULL) {
        return;
    }
    free(engine->items);
    free(engine);
}

/* ---------------- Цикл инициализации ---------------- */

long long openxiso_boot_step_duration_ms(void) {
    return OPENXISO_STEP_DURATION_MS;
}

int openxiso_boot_total_steps(void) {
    /* Бюджет в 20 лет сохранён в архитектуре и включается одним флагом. */
    if (OPENXISO_ENDLESS_BOOT_SIMULATION) {
        const long long budget_ms = OPENXISO_MAX_BOOT_YEARS * OPENXISO_DAYS_PER_YEAR
                                    * 24LL * 60LL * 60LL * 1000LL;
        const long long steps = budget_ms / OPENXISO_STEP_DURATION_MS;
        return steps > (long long)INT_MAX ? INT_MAX : (int)steps;
    }
    return OPENXISO_BOOT_MESSAGE_COUNT * OPENXISO_VISIBLE_BOOT_CYCLES;
}

void openxiso_boot_enter_step(OpenXisoEngine* engine, int index) {
    if (engine == NULL) {
        return;
    }
    const int safe_index = index < 0 ? 0 : index;
    engine->boot_step_index = safe_index;
    engine->boot_stage = kBootMessages[safe_index % OPENXISO_BOOT_MESSAGE_COUNT].stage;
    engine->last_status = OPENXISO_OK;
}

void openxiso_boot_complete(OpenXisoEngine* engine) {
    if (engine != NULL) {
        engine->boot_complete = 1;
        engine->last_status = OPENXISO_OK;
    }
}

int openxiso_boot_is_complete(const OpenXisoEngine* engine) {
    return engine != NULL && engine->boot_complete;
}

int openxiso_boot_step_index(const OpenXisoEngine* engine) {
    return engine == NULL ? 0 : engine->boot_step_index;
}

OpenXisoBootStage openxiso_boot_stage(const OpenXisoEngine* engine) {
    return engine == NULL ? OPENXISO_STAGE_ARCHITECTURE : engine->boot_stage;
}

const char* openxiso_boot_message(const OpenXisoEngine* engine) {
    if (engine == NULL) {
        return "";
    }
    return kBootMessages[engine->boot_stage].text;
}

/* ---------------- Черновик конфигурации ---------------- */

void openxiso_draft_reset(OpenXisoEngine* engine) {
    if (engine == NULL) {
        return;
    }
    engine->os_name[0] = '\0';
    engine->image_uri[0] = '\0';
    engine->image_name[0] = '\0';
    engine->image_info[0] = '\0';
}

void openxiso_draft_set_os_name(OpenXisoEngine* engine, const char* utf8_name) {
    if (engine == NULL) {
        return;
    }
    copy_trimmed(engine->os_name, sizeof(engine->os_name), utf8_name);
    engine->last_status = OPENXISO_OK;
}

const char* openxiso_draft_os_name(const OpenXisoEngine* engine) {
    return engine == NULL ? "" : engine->os_name;
}

OpenXisoStatus openxiso_draft_select_image(OpenXisoEngine* engine,
                                           const char* uri,
                                           const char* display_name) {
    if (engine == NULL) {
        return OPENXISO_ERR_INVALID_ARGUMENT;
    }
    if (display_name == NULL || display_name[0] == '\0') {
        engine->last_status = OPENXISO_ERR_MISSING_FILENAME;
        return engine->last_status;
    }
    if (!ends_with_ignore_case(display_name, ".iso") &&
        !ends_with_ignore_case(display_name, ".img")) {
        engine->last_status = OPENXISO_ERR_UNSUPPORTED_IMAGE;
        return engine->last_status;
    }
    copy_text(engine->image_uri, sizeof(engine->image_uri), uri == NULL ? "" : uri);
    copy_text(engine->image_name, sizeof(engine->image_name), display_name);
    engine->image_info[0] = '\0';
    engine->last_status = OPENXISO_OK;
    return OPENXISO_OK;
}

const char* openxiso_draft_image_uri(const OpenXisoEngine* engine) {
    return engine == NULL ? "" : engine->image_uri;
}

const char* openxiso_draft_image_name(const OpenXisoEngine* engine) {
    return engine == NULL ? "" : engine->image_name;
}

int openxiso_draft_can_submit(const OpenXisoEngine* engine) {
    if (engine == NULL) {
        return 0;
    }
    return has_content(engine->os_name)
           && has_content(engine->image_uri)
           && has_content(engine->image_name);
}

/* ---------------- Чтение образа через дескриптор из Java ---------------- */

OpenXisoStatus openxiso_probe_image(OpenXisoEngine* engine, int fd,
                                    long long size, char* out, size_t out_capacity) {
    if (engine == NULL || out == NULL || out_capacity == 0) {
        return OPENXISO_ERR_INVALID_ARGUMENT;
    }
    out[0] = '\0';
    engine->image_info[0] = '\0';
    if (fd < 0) {
        engine->last_status = OPENXISO_ERR_IO;
        return engine->last_status;
    }

    unsigned char sector[2048];
    /* Первичный дескриптор тома ISO 9660 располагается в секторе 16. */
    const ssize_t sector_bytes = pread(fd, sector, sizeof(sector), (off_t)(16 * 2048));
    if (sector_bytes == (ssize_t)sizeof(sector) &&
        sector[0] == 1 && memcmp(sector + 1, "CD001", 5) == 0) {
        char volume_id[33];
        memcpy(volume_id, sector + 40, 32);
        volume_id[32] = '\0';
        size_t id_length = strlen(volume_id);
        while (id_length > 0 &&
               (volume_id[id_length - 1] == ' ' || volume_id[id_length - 1] == '\0')) {
            volume_id[--id_length] = '\0';
        }
        const uint32_t volume_sectors = read_le32(sector + 80);
        const long long volume_bytes = volume_sectors > 0
                                       ? (long long)volume_sectors * 2048LL
                                       : size;
        snprintf(engine->image_info, sizeof(engine->image_info),
                 "ISO 9660 · «%s» · %lld МБ",
                 volume_id[0] == '\0' ? "Без метки" : volume_id,
                 megabytes(volume_bytes));
    } else {
        /* Сырой образ .img: проверяем сигнатуру загрузочной записи 0x55AA. */
        unsigned char boot[512];
        const ssize_t boot_bytes = pread(fd, boot, sizeof(boot), (off_t)0);
        const int has_boot_record = boot_bytes == (ssize_t)sizeof(boot)
                                    && boot[510] == 0x55 && boot[511] == 0xAA;
        snprintf(engine->image_info, sizeof(engine->image_info),
                 has_boot_record ? "Образ диска с загрузочной записью · %lld МБ"
                                 : "Сырой образ диска · %lld МБ",
                 megabytes(size));
    }

    copy_text(out, out_capacity, engine->image_info);
    engine->last_status = OPENXISO_OK;
    return OPENXISO_OK;
}

/* ---------------- Список конфигураций ---------------- */

long long openxiso_create_configuration(OpenXisoEngine* engine) {
    if (engine == NULL) {
        return -1;
    }
    if (!openxiso_draft_can_submit(engine)) {
        engine->last_status = OPENXISO_ERR_FORM_INCOMPLETE;
        return -1;
    }
    if (engine->count == engine->capacity) {
        const size_t next_capacity = engine->capacity == 0 ? 4 : engine->capacity * 2;
        OpenXisoConfiguration* grown = (OpenXisoConfiguration*)realloc(
            engine->items, next_capacity * sizeof(OpenXisoConfiguration));
        if (grown == NULL) {
            engine->last_status = OPENXISO_ERR_STORAGE;
            return -1;
        }
        engine->items = grown;
        engine->capacity = next_capacity;
    }

    OpenXisoConfiguration* item = &engine->items[engine->count++];
    item->id = engine->next_id++;
    copy_text(item->os_name, sizeof(item->os_name), engine->os_name);
    copy_text(item->image_uri, sizeof(item->image_uri), engine->image_uri);
    copy_text(item->image_name, sizeof(item->image_name), engine->image_name);
    copy_text(item->image_info, sizeof(item->image_info), engine->image_info);

    const long long created_id = item->id;
    openxiso_draft_reset(engine);
    engine->last_status = OPENXISO_OK;
    return created_id;
}

int openxiso_configuration_count(const OpenXisoEngine* engine) {
    if (engine == NULL || engine->count > (size_t)INT_MAX) {
        return 0;
    }
    return (int)engine->count;
}

static const OpenXisoConfiguration* configuration_at(const OpenXisoEngine* engine, int index) {
    if (engine == NULL || index < 0 || (size_t)index >= engine->count) {
        return NULL;
    }
    return &engine->items[index];
}

const char* openxiso_configuration_os_name(const OpenXisoEngine* engine, int index) {
    const OpenXisoConfiguration* item = configuration_at(engine, index);
    return item == NULL ? "" : item->os_name;
}

const char* openxiso_configuration_image_name(const OpenXisoEngine* engine, int index) {
    const OpenXisoConfiguration* item = configuration_at(engine, index);
    return item == NULL ? "" : item->image_name;
}

const char* openxiso_configuration_image_uri(const OpenXisoEngine* engine, int index) {
    const OpenXisoConfiguration* item = configuration_at(engine, index);
    return item == NULL ? "" : item->image_uri;
}

/* ---------------- Диагностика ---------------- */

OpenXisoStatus openxiso_last_status(const OpenXisoEngine* engine) {
    return engine == NULL ? OPENXISO_ERR_INVALID_ARGUMENT : engine->last_status;
}

const char* openxiso_status_message(OpenXisoStatus status) {
    const int index = (int)status;
    const int count = (int)(sizeof(kStatusMessages) / sizeof(kStatusMessages[0]));
    if (index < 0 || index >= count) {
        return "Неизвестная ошибка нативного движка";
    }
    return kStatusMessages[index];
}
