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

    static native String statusMessage(int status);
}
