package com.openxiso.app;

/*
 * NativeBridge.java — единственная точка входа в нативный код.
 *
 * Загружает libopenxiso.so (собирается из app/src/main/cpp/CMakeLists.txt)
 * и хранит дескриптор движка в статическом поле, чтобы состояние на C не
 * терялось при повороте экрана: Activity пересоздаётся, процесс — нет.
 * Все native-методы реализованы в openxiso_jni.cpp и взаимно однозначно
 * соответствуют функциям из include/openxiso_core.h.
 * Все сообщения об ошибках приходят из C уже на русском языке.
 */

public final class NativeBridge {

    /** Код успешного выполнения (OPENXISO_OK из openxiso_core.h). */
    public static final int STATUS_OK = 0;

    /* Разрядность гостя: выбирается в диалоге конфигурации. */
    public static final int CPU_MODE_16 = 0;
    public static final int CPU_MODE_32 = 1;
    public static final int CPU_MODE_64 = 2;

    /* Состояния виртуального PC (PcMachineStatus из pc_machine.h). */
    public static final int EMULATOR_IDLE = 0;
    public static final int EMULATOR_RUNNING = 1;
    public static final int EMULATOR_HALTED = 2;
    public static final int EMULATOR_ERROR = 3;
    public static final int EMULATOR_WAITING = 4;

    static {
        System.loadLibrary("openxiso");
    }

    private static long engineHandle = 0L;

    private NativeBridge() {
        // Класс содержит только статические методы.
    }

    /** Возвращает движок процесса, создавая его при первом обращении. */
    public static synchronized long engine() {
        if (engineHandle == 0L) {
            engineHandle = createEngine();
            if (engineHandle == 0L) {
                throw new IllegalStateException("Не удалось создать нативный движок OpenXiso");
            }
        }
        return engineHandle;
    }

    /** Освобождает движок при окончательном закрытии приложения. */
    public static synchronized void releaseEngine() {
        if (engineHandle != 0L) {
            destroyEngine(engineHandle);
            engineHandle = 0L;
        }
    }

    private static native long createEngine();

    private static native void destroyEngine(long handle);

    static native long getStepDurationMs();

    static native int getTotalBootSteps();

    static native void enterBootStep(long handle, int index);

    static native String getBootMessage(long handle);

    /** Стадия загрузки 0..4: Java выбирает по ней типографику сообщения. */
    static native int getBootStage(long handle);

    static native void completeBoot(long handle);

    static native boolean isBootComplete(long handle);

    static native void setOsName(long handle, String name);

    static native String getOsName(long handle);

    /** Возвращает код OpenXisoStatus; STATUS_OK означает принятый файл. */
    static native int selectImage(long handle, String uri, String displayName);

    static native String getImageName(long handle);

    static native String getImageUri(long handle);

    static native boolean canSubmit(long handle);

    /** Читает уже открытый Java дескриптор и возвращает описание образа. */
    static native String probeImage(long handle, int fd, long size);

    /** Идентификатор новой конфигурации или -1, если форма не заполнена. */
    static native long createConfiguration(long handle);

    static native int getConfigurationCount(long handle);

    static native String getConfigurationOsName(long handle, int index);

    static native String getConfigurationFileName(long handle, int index);

    static native String getConfigurationImageUri(long handle, int index);

    static native void resetDraft(long handle);

    static native void setCpuMode(long handle, int mode);

    static native int getConfigurationCpuMode(long handle, int index);

    static native String statusMessage(int status);

    /*
     * Настоящий эмулятор: интерпретатор x86 real mode + собственный Legacy BIOS.
     * machine — отдельный нативный дескриптор, не зависящий от engineHandle;
     * дескриптор файла образа открывает Java через SAF и удерживает открытым.
     */
    static native long emulatorCreate();

    static native void emulatorDestroy(long machine);

    static native int emulatorLoadImage(long machine, int fd, long size);

    static native int emulatorSetCpuMode(long machine, int mode);

    static native int emulatorStart(long machine);

    static native int emulatorRunSlice(long machine, int instructions);

    /** 25 строк VGA-текста 80x25 в UTF-8, разделённые '\n'. */
    static native String emulatorScreen(long machine);

    static native int emulatorStatus(long machine);

    static native String emulatorLog(long machine);

    static native String emulatorError(long machine);

    /** Младший байт — ASCII в AL, старший — скан-код в AH. */
    static native void emulatorPushKey(long machine, int scanAndAscii);

    static native void emulatorStop(long machine);

    static native long emulatorInstructions(long machine);

    static native String emulatorFirmware();
}
