/*
 * emulator_jni.cpp — JNI-мост к настоящему виртуальному PC.
 *
 * Бизнес-логики здесь нет: только преобразование дескрипторов и строк между
 * Java и C-ядром (include/pc_machine.h). Эмулятор x86, El Torito/MBR и
 * собственный маленький Legacy BIOS живут в C-файлах и собираются отдельно.
 * Все сообщения журнала — на русском языке.
 */

#include <jni.h>
#include <android/log.h>

#include <cstdint>
#include <string>

#include "include/legacy_bios.h"
#include "include/pc_machine.h"

namespace {

constexpr const char* LOG_TAG = "OpenXiso";

PcMachine* machineFromHandle(jlong handle) {
    return reinterpret_cast<PcMachine*>(static_cast<intptr_t>(handle));
}

jlong handleFromMachine(PcMachine* machine) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(machine));
}

jstring javaString(JNIEnv* env, const char* value) {
    return value == nullptr ? nullptr : env->NewStringUTF(value);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_openxiso_app_NativeBridge_emulatorCreate(JNIEnv*, jclass) {
    PcMachine* machine = openxiso_machine_create();
    if (machine == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                            "Не удалось выделить 1 МБ памяти виртуального PC");
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                        "Виртуальный PC создан: %s", legacy_bios_version());
    return handleFromMachine(machine);
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_emulatorDestroy(JNIEnv*, jclass, jlong handle) {
    openxiso_machine_destroy(machineFromHandle(handle));
}

/** fd принадлежит ParcelFileDescriptor в Java и живёт всё время эмуляции. */
JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_emulatorLoadImage(JNIEnv*, jclass, jlong handle,
                                                     jint fd, jlong size) {
    return openxiso_machine_load_image(machineFromHandle(handle), fd,
                                       static_cast<long long>(size));
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_emulatorStart(JNIEnv*, jclass, jlong handle) {
    return openxiso_machine_start(machineFromHandle(handle));
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_emulatorRunSlice(JNIEnv*, jclass, jlong handle,
                                                    jint instructions) {
    if (instructions <= 0) {
        return 0;
    }
    return openxiso_machine_run_slice(machineFromHandle(handle),
                                      static_cast<uint32_t>(instructions));
}

/** 25 строк VGA-текста 80x25: CP866 уже переведён в UTF-8 в C-ядре. */
JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_emulatorScreen(JNIEnv* env, jclass,
                                                  jlong handle) {
    const PcMachine* machine = machineFromHandle(handle);
    if (machine == nullptr) {
        return javaString(env, "");
    }
    std::string screen;
    screen.reserve(OPENXISO_SCREEN_ROWS * (OPENXISO_SCREEN_COLUMNS + 1));
    for (int row = 0; row < OPENXISO_SCREEN_ROWS; ++row) {
        char line[OPENXISO_SCREEN_COLUMNS * 3 + 8];
        if (openxiso_machine_get_screen_line(machine, row, line, sizeof(line)) < 0) {
            break;
        }
        screen += line;
        screen += '\n';
    }
    return javaString(env, screen.c_str());
}

JNIEXPORT jint JNICALL
Java_com_openxiso_app_NativeBridge_emulatorStatus(JNIEnv*, jclass, jlong handle) {
    return static_cast<jint>(openxiso_machine_status(machineFromHandle(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_emulatorLog(JNIEnv* env, jclass, jlong handle) {
    return javaString(env, openxiso_machine_log(machineFromHandle(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_emulatorError(JNIEnv* env, jclass, jlong handle) {
    return javaString(env, openxiso_machine_error(machineFromHandle(handle)));
}

/** Младший байт — ASCII в AL, старший — скан-код в AH (как в INT 16h). */
JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_emulatorPushKey(JNIEnv*, jclass, jlong handle,
                                                   jint scanAndAscii) {
    openxiso_machine_push_key(machineFromHandle(handle),
                              static_cast<uint16_t>(scanAndAscii & 0xFFFF));
}

JNIEXPORT void JNICALL
Java_com_openxiso_app_NativeBridge_emulatorStop(JNIEnv*, jclass, jlong handle) {
    openxiso_machine_stop(machineFromHandle(handle));
}

JNIEXPORT jlong JNICALL
Java_com_openxiso_app_NativeBridge_emulatorInstructions(JNIEnv*, jclass,
                                                       jlong handle) {
    return static_cast<jlong>(openxiso_machine_instructions(machineFromHandle(handle)));
}

JNIEXPORT jstring JNICALL
Java_com_openxiso_app_NativeBridge_emulatorFirmware(JNIEnv* env, jclass) {
    return javaString(env, legacy_bios_version());
}

}  // extern "C"
