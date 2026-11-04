#ifndef OPENXISO_CORE_H
#define OPENXISO_CORE_H

/*
 * openxiso_core.h — публичный C-интерфейс ядра OpenXiso (чистый C11).
 *
 * ГДЕ ЧТО ЛЕЖИТ (полная карта проекта — дублируется в README.md):
 *   app/src/main/java/com/openxiso/app/MainActivity.java  — интерфейс на Java, SAF, Handler-цикл
 *   app/src/main/java/com/openxiso/app/EmulatorUiState.java — зеркало UI-флагов и Bundle
 *   app/src/main/java/com/openxiso/app/NativeBridge.java  — объявления JNI и владелец движка
 *   app/src/main/java/com/openxiso/app/UiKit.java         — фабрика View, форм и ripple
 *   app/src/main/cpp/include/openxiso_core.h             — этот заголовок
 *   app/src/main/cpp/openxiso_core.c                    — состояние, цикл загрузки, разбор ISO 9660
 *   app/src/main/cpp/openxiso_jni.cpp                   — мост JNI на C++17
 *   app/src/main/cpp/CMakeLists.txt                     — сборка libopenxiso.so через NDK/CMake
 *   app/src/main/cpp/tests/test_core.c + Makefile       — хостовые тесты ядра (gcc, без NDK)
 *   app/src/main/res/...                                — строки, цвета, темы, XML-разметка, drawable
 *
 * Разделение обязанностей: Java отвечает за экран и получение файла из хранилища
 * телефона (ACTION_OPEN_DOCUMENT, ContentResolver, ParcelFileDescriptor); ядро на C
 * владеет состоянием, проверками и чтением образа через файловый дескриптор;
 * C++ лишь переводит вызовы JNI в вызовы этого C-интерфейса.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- Параметры цикла инициализации ---------------- */

/** Длительность одного сообщения загрузчика, мс. */
#define OPENXISO_STEP_DURATION_MS 2500LL

/** Номинальный предел эмуляции загрузки, лет. */
#define OPENXISO_MAX_BOOT_YEARS 20LL
#define OPENXISO_DAYS_PER_YEAR 365LL

/** Число низкоуровневых сообщений и число циклов в обычном режиме. */
#define OPENXISO_BOOT_MESSAGE_COUNT 5
#define OPENXISO_VISIBLE_BOOT_CYCLES 1

/*
 * 1 = демонстрационный «бесконечный» режим на 20 лет (252 288 000 шагов).
 * 0 = рабочий режим: один цикл сообщений, затем явное завершение инициализации.
 * Бесконечный режим по умолчанию выключен: иначе рабочее пространство,
 * плитка «плюс» и диалог никогда не стали бы доступны пользователю.
 */
#define OPENXISO_ENDLESS_BOOT_SIMULATION 0

/* ---------------- Ограничения буферов ---------------- */

#define OPENXISO_NAME_CAPACITY 256
#define OPENXISO_URI_CAPACITY 1024
#define OPENXISO_INFO_CAPACITY 256

/* ---------------- Коды возврата ---------------- */

typedef enum OpenXisoStatus {
    OPENXISO_OK = 0,
    OPENXISO_ERR_INVALID_ARGUMENT = 1,
    OPENXISO_ERR_MISSING_FILENAME = 2,
    OPENXISO_ERR_UNSUPPORTED_IMAGE = 3,
    OPENXISO_ERR_FORM_INCOMPLETE = 4,
    OPENXISO_ERR_STORAGE = 5,
    OPENXISO_ERR_IO = 6
} OpenXisoStatus;

/** Стадии загрузки определяют типографику сообщения в Java-интерфейсе. */
typedef enum OpenXisoBootStage {
    OPENXISO_STAGE_ARCHITECTURE = 0,
    OPENXISO_STAGE_JIT_CORE = 1,
    OPENXISO_STAGE_MEMORY = 2,
    OPENXISO_STAGE_STORAGE = 3,
    OPENXISO_STAGE_DEVICES = 4
} OpenXisoBootStage;

/** Непрозрачный дескриптор движка; память принадлежит ядру. */
typedef struct OpenXisoEngine OpenXisoEngine;

/* ---------------- Жизненный цикл движка ---------------- */

OpenXisoEngine* openxiso_engine_create(void);
void openxiso_engine_destroy(OpenXisoEngine* engine);

/* ---------------- Цикл инициализации ---------------- */

long long openxiso_boot_step_duration_ms(void);
int openxiso_boot_total_steps(void);
void openxiso_boot_enter_step(OpenXisoEngine* engine, int index);
void openxiso_boot_complete(OpenXisoEngine* engine);
int openxiso_boot_is_complete(const OpenXisoEngine* engine);
int openxiso_boot_step_index(const OpenXisoEngine* engine);
OpenXisoBootStage openxiso_boot_stage(const OpenXisoEngine* engine);

/** Сообщение в UTF-8 на русском языке. Строка принадлежит ядру, копировать не нужно. */
const char* openxiso_boot_message(const OpenXisoEngine* engine);

/* ---------------- Черновик конфигурации ---------------- */

void openxiso_draft_reset(OpenXisoEngine* engine);
void openxiso_draft_set_os_name(OpenXisoEngine* engine, const char* utf8_name);
const char* openxiso_draft_os_name(const OpenXisoEngine* engine);

/** Проверяет расширение .iso / .img и запоминает Uri и имя файла. */
OpenXisoStatus openxiso_draft_select_image(OpenXisoEngine* engine,
                                           const char* uri,
                                           const char* display_name);

const char* openxiso_draft_image_uri(const OpenXisoEngine* engine);
const char* openxiso_draft_image_name(const OpenXisoEngine* engine);
int openxiso_draft_can_submit(const OpenXisoEngine* engine);

/**
 * Читает образ через файловый дескриптор, открытый Java-стороной.
 * Распознаёт первичный дескриптор тома ISO 9660 (сектор 16) и загрузочную
 * запись 0x55AA; возвращает описание на русском языке в out.
 */
OpenXisoStatus openxiso_probe_image(OpenXisoEngine* engine, int fd,
                                    long long size, char* out, size_t out_capacity);

/* ---------------- Список конфигураций ---------------- */

/** Возвращает идентификатор созданной конфигурации или -1 при ошибке. */
long long openxiso_create_configuration(OpenXisoEngine* engine);
int openxiso_configuration_count(const OpenXisoEngine* engine);
const char* openxiso_configuration_os_name(const OpenXisoEngine* engine, int index);
const char* openxiso_configuration_image_name(const OpenXisoEngine* engine, int index);
const char* openxiso_configuration_image_uri(const OpenXisoEngine* engine, int index);

/* ---------------- Диагностика ---------------- */

OpenXisoStatus openxiso_last_status(const OpenXisoEngine* engine);
/** Текст ошибки на русском языке для показа в интерфейсе. */
const char* openxiso_status_message(OpenXisoStatus status);

#ifdef __cplusplus
}
#endif

#endif /* OPENXISO_CORE_H */
