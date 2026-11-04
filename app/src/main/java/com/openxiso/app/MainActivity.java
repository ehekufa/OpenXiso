package com.openxiso.app;

/*
 * ============================================================================
 * OpenXiso. КОММЕНТАРИЙ «ГДЕ ВСЁ БУДЕТ» — карта проекта для сборщика
 * ============================================================================
 *
 * Этот файл кладём в:  app/src/main/java/com/openxiso/app/MainActivity.java
 * Рядом с ним лежат:   EmulatorUiState.java  — флаги экрана и Bundle
 *                      EmulatorActivity.java — консоль настоящего эмулятора
 *                      NativeBridge.java     — объявления JNI + владелец движка
 *                      UiKit.java            — фабрика View, форм и ripple
 *
 * Нативный код (C и C++ вместе):
 *   app/src/main/cpp/include/openxiso_core.h  — публичный C-API ядра конфигураций
 *   app/src/main/cpp/openxiso_core.c          — состояние, цикл загрузки, ISO 9660
 *   app/src/main/cpp/include/x86_cpu.h        — интерпретатор реального режима 8086
 *   app/src/main/cpp/x86_cpu.c                — настоящие машинные коды, ModRM, INT
 *   app/src/main/cpp/boot_image.c             — разбор El Torito / MBR, чтение через fd
 *   app/src/main/cpp/legacy_bios.c            — СОБСТВЕННЫЙ маленький Legacy BIOS
 *   app/src/main/cpp/pc_machine.c             — 1 МБ ОЗУ, VGA 80x25, порты, клавиатура
 *   app/src/main/cpp/openxiso_jni.cpp         — мост JNI конфигураций (C++17)
 *   app/src/main/cpp/emulator_jni.cpp         — мост JNI виртуального PC (C++17)
 *   app/src/main/cpp/CMakeLists.txt           — сборка libopenxiso.so
 *   app/src/main/cpp/tests/                   — тесты ядра И реальной загрузки (gcc)
 *
 * Разметка и ресурсы:
 *   app/src/main/res/layout/activity_main.xml            — загрузчик + рабочее пространство
 *   app/src/main/res/layout/dialog_configuration.xml    — модальное окно 28 dp
 *   app/src/main/res/drawable/*.xml                     — «плюс», документ, формы, пунктир
 *   app/src/main/res/values/strings.xml                 — все строки на русском языке
 *   app/src/main/res/values/colors.xml, themes.xml       — палитра и тема
 *
 * Сборка (у себя, не в песочнице): Android Studio + NDK + CMake, затем
 *   ./gradlew :app:assembleDebug
 * APK появится здесь:  app/build/outputs/apk/debug/app-debug.apk
 * Полная инструкция — в README.md в корне репозитория.
 *
 * РАЗДЕЛЕНИЕ ОБЯЗАННОСТЕЙ (как и просили):
 *   Java  — весь интерфейс и ЗАГРУЗКА ФАЙЛОВ ИЗ ТЕЛЕФОНА: ACTION_OPEN_DOCUMENT,
 *           ContentResolver, OpenableColumns, ParcelFileDescriptor, передача fd в C.
 *           Плитка конфигурации запускает EmulatorActivity с настоящим эмулятором.
 *   C     — состояние, флаги, проверки, список конфигураций, чтение образа,
 *           интерпретатор x86, El Torito/MBR, ROM/IVT/POST и сервисы INT BIOS.
 *   C++   — только мост JNI без бизнес-логики.
 *
 * Все комментарии, сообщения журнала и строки интерфейса — на русском языке,
 * идентификаторы кода — на английском.
 * ============================================================================
 */

import android.animation.ArgbEvaluator;
import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.Dialog;
import android.content.Intent;
import android.content.res.ColorStateList;
import android.database.Cursor;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.util.TypedValue;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.GridLayout;
import android.widget.TextView;

import java.io.IOException;

public final class MainActivity extends Activity {

    private static final String TAG = "OpenXiso";
    private static final String KEY_UI_STATE = "openxiso.uiState";
    private static final int REQUEST_OPEN_IMAGE = 1001;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private long engine;
    private EmulatorUiState state;

    /* Вьюхи экранов. */
    private View loaderScreen;
    private View workspaceScreen;
    private TextView bootMessage;
    private TextView bootStep;
    private GridLayout workspaceGrid;
    private TextView configurationCounter;

    /* Вьюхи модального окна (пересоздаются вместе с Dialog). */
    private Dialog configurationDialog;
    private TextView osNameLabel;
    private EditText osNameInput;
    private GradientDrawable fieldOutline;
    private View fileBox;
    private TextView fileSelected;
    private TextView imageInfo;
    private TextView fileError;
    private Button createButton;
    private int currentFieldColor;
    private ValueAnimator fieldAnimator;

    /*
     * Корутинам из предыдущей версии здесь соответствует Handler-цикл:
     * каждые 2,5 с показывается следующее сообщение из C-ядра, а по достижении
     * openxiso_boot_total_steps() вызывается completeBoot() и открывается сетка.
     * Если в openxiso_core.h включить OPENXISO_ENDLESS_BOOT_SIMULATION = 1,
     * этот же цикл будет работать 20 лет (бюджет рассчитан в C).
     */
    private final Runnable bootStepRunnable = new Runnable() {
        @Override
        public void run() {
            if (state == null || state.bootComplete) {
                showWorkspace();
                return;
            }
            final int totalSteps = NativeBridge.getTotalBootSteps();
            if (state.bootStepIndex >= totalSteps) {
                NativeBridge.completeBoot(engine);
                state.bootComplete = true;
                showWorkspace();
                return;
            }
            NativeBridge.enterBootStep(engine, state.bootStepIndex);
            renderBootStep();
            state.bootStepIndex += 1;
            mainHandler.postDelayed(this, NativeBridge.getStepDurationMs());
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        engine = NativeBridge.engine();
        setContentView(R.layout.activity_main);

        loaderScreen = findViewById(R.id.loaderScreen);
        workspaceScreen = findViewById(R.id.workspaceScreen);
        bootMessage = findViewById(R.id.bootMessage);
        bootStep = findViewById(R.id.bootStep);
        workspaceGrid = findViewById(R.id.workspaceGrid);
        configurationCounter = findViewById(R.id.configurationCounter);

        final Bundle saved = savedInstanceState == null
                ? null
                : savedInstanceState.getBundle(KEY_UI_STATE);
        state = EmulatorUiState.restore(saved, engine);

        configureGridColumns();
        bootMessage.setText(NativeBridge.getBootMessage(engine));
        bootStep.setText(getString(R.string.boot_step,
                state.bootStepIndex + 1, NativeBridge.getTotalBootSteps()));

        if (state.bootComplete) {
            showWorkspace();
        } else {
            loaderScreen.setVisibility(View.VISIBLE);
            workspaceScreen.setVisibility(View.GONE);
        }
        if (state.dialogVisible) {
            showConfigurationDialog();
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        // Продолжаем загрузку с сохранённого шага; removeCallbacks исключает
        // дублирование цикла при повторном onStart.
        if (!state.bootComplete) {
            startBootLoop();
        }
    }

    @Override
    protected void onStop() {
        super.onStop();
        mainHandler.removeCallbacks(bootStepRunnable);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putBundle(KEY_UI_STATE, state.save());
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mainHandler.removeCallbacksAndMessages(null);
        if (fieldAnimator != null) {
            fieldAnimator.cancel();
        }
        if (configurationDialog != null && configurationDialog.isShowing()) {
            configurationDialog.setOnDismissListener(null);
            configurationDialog.dismiss();
        }
        // Движок живёт в процессе, поэтому освобождаем его только при закрытии
        // приложения, а не при повороте экрана.
        if (isFinishing()) {
            NativeBridge.releaseEngine();
        }
    }

    /* ---------------- Экран 1. Системный инициализатор ---------------- */

    private void startBootLoop() {
        mainHandler.removeCallbacks(bootStepRunnable);
        mainHandler.post(bootStepRunnable);
    }

    private void renderBootStep() {
        bootMessage.setText(NativeBridge.getBootMessage(engine));
        bootStep.setText(getString(R.string.boot_step,
                state.bootStepIndex + 1, NativeBridge.getTotalBootSteps()));

        /* Типографическая логика состояний: первые стадии крупнее, финальные — тоньше. */
        final int stage = NativeBridge.getBootStage(engine);
        final float sizeSp = stage <= 1 ? 16f : (stage == 2 ? 15f : 14f);
        bootMessage.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        bootMessage.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        bootMessage.setAlpha(0f);
        bootMessage.animate().alpha(1f).setDuration(320).start();
    }

    /* ---------------- Экран 2. Рабочее пространство ---------------- */

    private void showWorkspace() {
        mainHandler.removeCallbacks(bootStepRunnable);
        loaderScreen.setVisibility(View.GONE);
        workspaceScreen.setVisibility(View.VISIBLE);
        renderWorkspaceGrid();
    }

    /** Число колонок подстраивается под ширину экрана: 120 dp плитка + 16 dp зазор. */
    private void configureGridColumns() {
        final float widthDp = getResources().getDisplayMetrics().widthPixels
                / getResources().getDisplayMetrics().density;
        final int columns = Math.max(1, (int) ((widthDp - 48f) / 136f));
        workspaceGrid.setColumnCount(columns);
    }

    private void renderWorkspaceGrid() {
        workspaceGrid.removeAllViews();
        final int count = NativeBridge.getConfigurationCount(engine);
        for (int index = 0; index < count; index++) {
            final String osName = NativeBridge.getConfigurationOsName(engine, index);
            final String imageUri = NativeBridge.getConfigurationImageUri(engine, index);
            workspaceGrid.addView(UiKit.createConfigurationTile(this, osName,
                    NativeBridge.getConfigurationFileName(engine, index),
                    view -> startActivity(EmulatorActivity.launchIntent(
                            MainActivity.this, osName, imageUri))));
        }
        workspaceGrid.addView(UiKit.createPlusTile(
                this, view -> showConfigurationDialog()));
        configurationCounter.setText(getString(R.string.workspace_counter, count));
    }

    /* ---------------- Экран 3. Модальное окно конфигурации ---------------- */

    private void showConfigurationDialog() {
        if (configurationDialog != null && configurationDialog.isShowing()) {
            return;
        }
        state.dialogVisible = true;

        final View content = getLayoutInflater().inflate(R.layout.dialog_configuration, null);
        final Dialog dialog = new Dialog(this);
        configurationDialog = dialog;
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE);
        dialog.setContentView(content);

        final Window window = dialog.getWindow();
        if (window != null) {
            // Белое плавающее окно со скруглением 28 dp задано в bg_dialog.xml.
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setDimAmount(0.32f);
        }

        bindDialogViews(content);
        dialog.setOnDismissListener(listener -> {
            state.dialogVisible = false;
            configurationDialog = null;
        });
        dialog.show();
        if (window != null) {
            window.setLayout(WindowManager.LayoutParams.MATCH_PARENT,
                    WindowManager.LayoutParams.WRAP_CONTENT);
            final WindowManager.LayoutParams attributes = window.getAttributes();
            attributes.horizontalMargin = 0.04f; // ≈ 24 dp по обоим краям
            window.setAttributes(attributes);
        }
        syncDialogFromState();
    }

    private void bindDialogViews(View root) {
        osNameLabel = root.findViewById(R.id.osNameLabel);
        osNameInput = root.findViewById(R.id.osNameInput);
        fileBox = root.findViewById(R.id.fileBox);
        fileSelected = root.findViewById(R.id.fileSelected);
        imageInfo = root.findViewById(R.id.imageInfo);
        fileError = root.findViewById(R.id.fileError);
        createButton = root.findViewById(R.id.createButton);
        final View cancelButton = root.findViewById(R.id.cancelButton);

        /* Плоское outlined-поле: анимация обводки в Google Blue при фокусе. */
        fieldOutline = UiKit.roundedRect(this, Color.TRANSPARENT,
                UiKit.color(this, R.color.outline), 12f, UiKit.dp(this, 1));
        osNameInput.setBackground(fieldOutline);
        currentFieldColor = UiKit.color(this, R.color.outline);
        setupCreateButton();

        osNameInput.setOnFocusChangeListener((view, hasFocus) -> animateFieldFocus(hasFocus));
        osNameInput.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
                // Не требуется: значение читается целиком в onTextChanged.
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
                state.osNameDraft = s.toString();
                NativeBridge.setOsName(engine, state.osNameDraft);
                updateSubmitButton();
            }

            @Override
            public void afterTextChanged(Editable s) {
                // Не требуется: форма уже синхронизирована с C-ядром.
            }
        });

        /* Синяя рябь поверх пунктирной рамки контейнера выбора файла. */
        fileBox.setForeground(UiKit.ripple(
                Color.argb(31, 26, 115, 232), new ColorDrawable(Color.TRANSPARENT)));

        /* SAF: именно Java открывает документ из хранилища телефона. */
        fileBox.setOnClickListener(view -> openDocumentPicker());
        cancelButton.setOnClickListener(view -> {
            if (configurationDialog != null) {
                configurationDialog.dismiss();
            }
        });
        createButton.setOnClickListener(view -> submitConfiguration());
    }

    private void setupCreateButton() {
        final int blue = UiKit.color(this, R.color.blue);
        final int grey = UiKit.color(this, R.color.secondary);
        final GradientDrawable enabled = UiKit.roundedRect(this, blue, blue, 20f, 0);
        final GradientDrawable disabled = UiKit.roundedRect(this, grey, grey, 20f, 0);
        final StateListDrawable states = new StateListDrawable();
        states.addState(new int[]{-android.R.attr.state_enabled}, disabled);
        states.addState(new int[]{}, enabled);
        createButton.setBackground(UiKit.ripple(Color.argb(48, 255, 255, 255), states));

        createButton.setTextColor(new ColorStateList(
                new int[][]{
                        new int[]{-android.R.attr.state_enabled},
                        new int[]{}
                },
                new int[]{
                        UiKit.color(this, R.color.text_secondary),
                        UiKit.color(this, R.color.surface)
                }));
    }

    private void animateFieldFocus(boolean focused) {
        final int target = focused
                ? UiKit.color(this, R.color.blue)
                : UiKit.color(this, R.color.outline);
        if (fieldAnimator != null) {
            fieldAnimator.cancel();
        }
        fieldAnimator = ValueAnimator.ofObject(new ArgbEvaluator(), currentFieldColor, target);
        fieldAnimator.setDuration(180);
        fieldAnimator.addUpdateListener(animation -> {
            currentFieldColor = (Integer) animation.getAnimatedValue();
            fieldOutline.setStroke(UiKit.dp(this, 1), currentFieldColor);
            if (focused) {
                osNameLabel.setTextColor(currentFieldColor);
            } else {
                osNameLabel.setTextColor(UiKit.color(this, R.color.text_secondary));
            }
            fieldOutline.invalidateSelf();
        });
        fieldAnimator.start();
    }

    private void updateSubmitButton() {
        if (createButton != null) {
            createButton.setEnabled(NativeBridge.canSubmit(engine));
        }
    }

    private void syncDialogFromState() {
        if (osNameInput == null) {
            return;
        }
        final String draft = state.osNameDraft == null ? "" : state.osNameDraft;
        if (!draft.contentEquals(osNameInput.getText())) {
            osNameInput.setText(draft);
            osNameInput.setSelection(osNameInput.getText().length());
        }

        if (state.imageName != null) {
            fileSelected.setVisibility(View.VISIBLE);
            fileSelected.setText(getString(R.string.file_selected, state.imageName));
        } else {
            fileSelected.setVisibility(View.GONE);
        }

        if (state.imageInfo != null) {
            imageInfo.setVisibility(View.VISIBLE);
            imageInfo.setText(getString(R.string.image_info, state.imageInfo));
        } else {
            imageInfo.setVisibility(View.GONE);
        }

        if (state.errorMessage != null) {
            fileError.setVisibility(View.VISIBLE);
            fileError.setText(state.errorMessage);
        } else {
            fileError.setVisibility(View.GONE);
        }
        updateSubmitButton();
    }

    /* ---------------- SAF: файлы образа из памяти телефона ---------------- */

    /*
     * Используется платформенный startActivityForResult: проект не зависит от
     * AndroidX, поэтому интерфейс целиком собран на системных View. При желании
     * вызов можно заменить на registerForActivityResult(OpenDocument) после
     * добавления androidx.activity:activity.
     */
    @SuppressWarnings("deprecation")
    private void openDocumentPicker() {
        final Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                .addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*")
                .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                        "application/octet-stream",
                        "application/x-iso9660-image",
                        "*/*"
                })
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                        | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQUEST_OPEN_IMAGE);
    }

    @Override
    @SuppressWarnings("deprecation")
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_OPEN_IMAGE || resultCode != RESULT_OK || data == null) {
            return;
        }
        final Uri uri = data.getData();
        if (uri == null) {
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(
                    uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException ignored) {
            Log.w(TAG, "Поставщик не выдал постоянное разрешение на чтение файла");
        }

        /* Имя файла берём у провайдера, расширение проверяет C-ядро. */
        final String displayName = resolveDisplayName(uri);
        final int status = NativeBridge.selectImage(engine, uri.toString(), displayName);
        if (status != NativeBridge.STATUS_OK) {
            state.imageUri = null;
            state.imageName = null;
            state.imageInfo = null;
            state.errorMessage = NativeBridge.statusMessage(status);
            syncDialogFromState();
            return;
        }

        state.imageUri = uri.toString();
        state.imageName = displayName;
        state.errorMessage = null;
        /* Java открывает дескриптор, C читает из него ISO 9660 / MBR. */
        state.imageInfo = probeImage(uri);
        syncDialogFromState();
    }

    private String resolveDisplayName(Uri uri) {
        final Cursor cursor = getContentResolver().query(uri, null, null, null, null);
        if (cursor == null) {
            return null;
        }
        try {
            final int nameIndex = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
            if (nameIndex >= 0 && cursor.moveToFirst()) {
                return cursor.getString(nameIndex);
            }
        } finally {
            cursor.close();
        }
        return null;
    }

    private String probeImage(Uri uri) {
        try (ParcelFileDescriptor descriptor =
                     getContentResolver().openFileDescriptor(uri, "r")) {
            if (descriptor == null) {
                return null;
            }
            return NativeBridge.probeImage(engine, descriptor.getFd(), descriptor.getStatSize());
        } catch (IOException | RuntimeException error) {
            Log.w(TAG, "Не удалось прочитать выбранный образ: " + error.getMessage());
            return null;
        }
    }

    /* ---------------- Создание конфигурации ---------------- */

    private void submitConfiguration() {
        final long createdId = NativeBridge.createConfiguration(engine);
        if (createdId < 0) {
            state.errorMessage = getString(R.string.error_form_incomplete);
            syncDialogFromState();
            return;
        }
        Log.i(TAG, "Создана конфигурация с идентификатором " + createdId);
        state.clearDraft();
        if (configurationDialog != null) {
            configurationDialog.dismiss();
        }
        renderWorkspaceGrid();
    }
}
