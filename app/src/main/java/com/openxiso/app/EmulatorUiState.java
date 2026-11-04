package com.openxiso.app;

/*
 * EmulatorUiState.java — зеркало UI-флагов и сохранение состояния экрана.
 *
 * Источник истины для данных (список конфигураций, черновик, проверки) —
 * нативное C-ядро. Здесь живут только то, что относится к представлению:
 * текущий шаг загрузчика, признак готовности, видимость диалога, выбранный
 * Uri и тексты для показа. Поля переживают поворот экрана через Bundle и
 * при восстановлении заново «накатываются» на движок (метод applyToNative).
 */

import android.os.Bundle;

final class EmulatorUiState {

    private static final String KEY_BOOT_COMPLETE = "openxiso.bootComplete";
    private static final String KEY_BOOT_STEP = "openxiso.bootStep";
    private static final String KEY_DIALOG_VISIBLE = "openxiso.dialogVisible";
    private static final String KEY_OS_NAME = "openxiso.osName";
    private static final String KEY_IMAGE_URI = "openxiso.imageUri";
    private static final String KEY_IMAGE_NAME = "openxiso.imageName";
    private static final String KEY_IMAGE_INFO = "openxiso.imageInfo";
    private static final String KEY_ERROR = "openxiso.error";
    private static final String KEY_CPU_MODE = "openxiso.cpuMode";

    boolean bootComplete;
    int bootStepIndex;
    boolean dialogVisible;
    String osNameDraft = "";
    String imageUri;
    String imageName;
    String imageInfo;
    String errorMessage;
    int cpuMode = NativeBridge.CPU_MODE_16;

    Bundle save() {
        final Bundle bundle = new Bundle();
        bundle.putBoolean(KEY_BOOT_COMPLETE, bootComplete);
        bundle.putInt(KEY_BOOT_STEP, bootStepIndex);
        bundle.putBoolean(KEY_DIALOG_VISIBLE, dialogVisible);
        bundle.putString(KEY_OS_NAME, osNameDraft);
        bundle.putString(KEY_IMAGE_URI, imageUri);
        bundle.putString(KEY_IMAGE_NAME, imageName);
        bundle.putString(KEY_IMAGE_INFO, imageInfo);
        bundle.putString(KEY_ERROR, errorMessage);
        bundle.putInt(KEY_CPU_MODE, cpuMode);
        return bundle;
    }

    static EmulatorUiState restore(Bundle saved, long engine) {
        final EmulatorUiState state = new EmulatorUiState();
        if (saved != null) {
            state.bootComplete = saved.getBoolean(KEY_BOOT_COMPLETE, false);
            state.bootStepIndex = saved.getInt(KEY_BOOT_STEP, 0);
            state.dialogVisible = saved.getBoolean(KEY_DIALOG_VISIBLE, false);
            state.osNameDraft = saved.getString(KEY_OS_NAME, "");
            state.imageUri = saved.getString(KEY_IMAGE_URI, null);
            state.imageName = saved.getString(KEY_IMAGE_NAME, null);
            state.imageInfo = saved.getString(KEY_IMAGE_INFO, null);
            state.errorMessage = saved.getString(KEY_ERROR, null);
            state.cpuMode = saved.getInt(KEY_CPU_MODE, NativeBridge.CPU_MODE_16);
        }
        state.applyToNative(engine);
        return state;
    }

    /** Синхронизирует сохранённые поля с движком после пересоздания Activity. */
    void applyToNative(long engine) {
        NativeBridge.setOsName(engine, osNameDraft);
        NativeBridge.setCpuMode(engine, cpuMode);
        if (imageUri != null && imageName != null) {
            NativeBridge.selectImage(engine, imageUri, imageName);
        }
        NativeBridge.enterBootStep(engine, bootStepIndex);
        if (bootComplete) {
            NativeBridge.completeBoot(engine);
        }
    }

    /** Очищает форму после успешного создания конфигурации. */
    void clearDraft() {
        osNameDraft = "";
        imageUri = null;
        imageName = null;
        imageInfo = null;
        errorMessage = null;
        cpuMode = NativeBridge.CPU_MODE_16;
        dialogVisible = false;
    }
}
