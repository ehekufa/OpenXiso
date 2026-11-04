/*
 * openxiso_jni.cpp — мост JNI на C++17.
 *
 * Файл не содержит бизнес-логики: он только переводит вызовы Java в вызовы
 * C-ядра (include/openxiso_core.h) и обратно. Именно поэтому в проекте
 * используются и C, и C++ вместе: ядро остаётся переносимым и тестируемым
 * обычным gcc, а мост пользуется std::string и RAII для строк JNI.
 * Все сообщения журнала — на русском языке.
 */

#include <jni.h>
#include <android/log.h>

#include <cstdint>

#include "include/openxiso_core.h"

#define OPENXISO_LOG_TAG "OpenXiso"
#define OPENXISO_LOG_INFO(...) \
    __android_log_print(ANDROID_LOG_INFO, OPENXISO_LOG_TAG, __VA_ARGS__)
#define OPENXISO_LOG_WARN(...) \
    __android_log_print(ANDROID_LOG_WARN, OPENXISO_LOG_TAG, __VA_ARGS__)

namespace {

/** Преобразование jlong <-> указатель с учётом 32- и 64-разрядных ABI. */
OpenXisoEngine* engineFromHandle(jlong handle) {
    return reinterpret_cast<OpenXisoEngine*>(static_cast<intptr_t>(handle));
}

jlong handleFromEngine(OpenXisoEngine* engine) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(engine));
}

/** RAII-обёртка над GetStringUTFChars: гарантирует освобождение буфера. */
class JniUtf8 {
public:
    JniUtf8(JNIEnv* env, jstring value)
        : env_(env),
          value_(value),
          chars_(value == nullptr ? nullptr : env->GetStringUTFChars(value, nullptr)) {}

    ~JniUtf8() {
        if (value_ != nullptr && chars_ != nullptr) {
            env_->ReleaseStringUTFChars(value_, chars_);
        }
    }

    JniUtf8(const JniUtf8&) = delete;
    JniUtf8& operator=(const JniUtf8&) = delete;

    const char* get() const { return chars_; }

private:
    JNIEnv* env_;
    jstring value_;
    const char* chars_;
};

jstring toJavaString(JNIEnv* env, const char* value) {
    if (value == nullptr) {
        return nullptr;
    }
    return env->NewStringUTF(value);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_openxiso_app_NativeBridge_createEngine(JNIEnv*, jclass) {
    OpenXisoEngine* engine = openxiso_engine_create();
    if (engine == nullptr) {
        OPENXISO_LOG_WARN("Не удалось выделить память под нативный движок");
        return 0;
    }
    OPENXISO_LOG_INFO("Нативный движок создан, шагов загрузки: %d",
                      openxiso_boot_total_steps());
    return handleFromEngine(engine);
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_destroyEngine(JNIEnv*, jclass, jlong handle) {
    openxiso_engine_destroy(engineFromHandle(handle));
    OPENXISO_LOG_INFO("Нативный движок освобождён");
}

JNIEXPORT jlong JNICALL
Java_com_openxiso_app_NativeBridge_getStepDurationMs(JNIEnv*, jclass) {
    return static_cast<jlong>(openxiso_boot_step_duration_ms());
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_getTotalBootSteps(JNIEnv*, jclass) {
    return static_cast<jint>(openxiso_boot_total_steps());
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_enterBootStep(JNIEnv*, jclass,
                                                 jlong handle, jint index) {
    openxiso_boot_enter_step(engineFromHandle(handle), static_cast<int>(index));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getBootMessage(JNIEnv* env, jclass, jlong handle) {
    return toJavaString(env, openxiso_boot_message(engineFromHandle(handle)));
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_getBootStage(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(openxiso_boot_stage(engineFromHandle(handle)));
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_completeBoot(JNIEnv*, jclass, jlong handle) {
    openxiso_boot_complete(engineFromHandle(handle));
    OPENXISO_LOG_INFO("Инициализация завершена, открыто рабочее пространство");
}

JNIEXPORT jboolean JNICALL
Java_com_openxiso_app_NativeBridge_isBootComplete(JNIEnv*, jclass, jlong handle) {
    return openxiso_boot_is_complete(engineFromHandle(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_setOsName(JNIEnv* env, jclass,
                                             jlong handle, jstring name) {
    JniUtf8 utf8(env, name);
    openxiso_draft_set_os_name(engineFromHandle(handle), utf8.get());
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getOsName(JNIEnv* env, jclass, jlong handle) {
    return toJavaString(env, openxiso_draft_os_name(engineFromHandle(handle)));
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_selectImage(JNIEnv* env, jclass,
                                               jlong handle, jstring uri,
                                               jstring displayName) {
    JniUtf8 utf8Uri(env, uri);
    JniUtf8 utf8Name(env, displayName);
    const OpenXisoStatus status = openxiso_draft_select_image(
        engineFromHandle(handle), utf8Uri.get(), utf8Name.get());
    if (status != OPENXISO_OK) {
        OPENXISO_LOG_WARN("Файл образа отклонён: %s", openxiso_status_message(status));
    }
    return static_cast<jint>(status);
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getImageName(JNIEnv* env, jclass, jlong handle) {
    return toJavaString(env, openxiso_draft_image_name(engineFromHandle(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getImageUri(JNIEnv* env, jclass, jlong handle) {
    return toJavaString(env, openxiso_draft_image_uri(engineFromHandle(handle)));
}

JNIEXPORT jboolean JNICALL
Java_com_openxiso_app_NativeBridge_canSubmit(JNIEnv*, jclass, jlong handle) {
    return openxiso_draft_can_submit(engineFromHandle(handle)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_probeImage(JNIEnv* env, jclass,
                                              jlong handle, jint fd, jlong size) {
    char description[OPENXISO_INFO_CAPACITY];
    const OpenXisoStatus status = openxiso_probe_image(
        engineFromHandle(handle), static_cast<int>(fd),
        static_cast<long long>(size), description, sizeof(description));
    if (status != OPENXISO_OK) {
        OPENXISO_LOG_WARN("Не удалось разобрать образ: %s",
                          openxiso_status_message(status));
        return nullptr;
    }
    OPENXISO_LOG_INFO("Образ прочитан: %s", description);
    return toJavaString(env, description);
}

JNIEXPORT jlong JNICALL
Java_com_openxiso_app_NativeBridge_createConfiguration(JNIEnv*, jclass, jlong handle) {
    const long long id = openxiso_create_configuration(engineFromHandle(handle));
    if (id < 0) {
        OPENXISO_LOG_WARN("Конфигурация не создана: форма заполнена не полностью");
    } else {
        OPENXISO_LOG_INFO("Создана конфигурация с идентификатором %lld", id);
    }
    return static_cast<jlong>(id);
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_getConfigurationCount(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(openxiso_configuration_count(engineFromHandle(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getConfigurationOsName(JNIEnv* env, jclass,
                                                          jlong handle, jint index) {
    return toJavaString(env, openxiso_configuration_os_name(
        engineFromHandle(handle), static_cast<int>(index)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getConfigurationFileName(JNIEnv* env, jclass,
                                                            jlong handle, jint index) {
    return toJavaString(env, openxiso_configuration_image_name(
        engineFromHandle(handle), static_cast<int>(index)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_getConfigurationImageUri(JNIEnv* env, jclass,
                                                             jlong handle, jint index) {
    return toJavaString(env, openxiso_configuration_image_uri(
        engineFromHandle(handle), static_cast<int>(index)));
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_resetDraft(JNIEnv*, jclass, jlong handle) {
    openxiso_draft_reset(engineFromHandle(handle));
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_setCpuMode(JNIEnv*, jclass, jlong handle,
                                              jint mode) {
    openxiso_draft_set_cpu_mode(engineFromHandle(handle), static_cast<int>(mode));
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_getConfigurationCpuMode(JNIEnv*, jclass,
                                                            jlong handle,
                                                            jint index) {
    return static_cast<jint>(openxiso_configuration_cpu_mode(
        engineFromHandle(handle), static_cast<int>(index)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_statusMessage(JNIEnv* env, jclass, jint status) {
    return toJavaString(env,
                        openxiso_status_message(static_cast<OpenXisoStatus>(status)));
}

}  // extern "C"
