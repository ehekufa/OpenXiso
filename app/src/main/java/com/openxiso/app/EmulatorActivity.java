package com.openxiso.app;

/*
 * EmulatorActivity.java — экран НАСТОЯЩЕЙ эмуляции, а не имитации.
 *
 * Java открывает выбранный SAF-документ и удерживает ParcelFileDescriptor,
 * пока работает виртуальный PC. C-ядро через этот fd читает ISO 9660 /
 * El Torito или MBR, исполняет реальные 16-битные инструкции загрузчика и
 * обслуживает INT собственным маленьким Legacy BIOS. Здесь فقط UI-цикл:
 * фоновый поток исполняет инструкции, главный поток рисует VGA-текст 80x25.
 *
 * Все строки интерфейса и журнала — на русском языке.
 */

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.view.KeyEvent;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public final class EmulatorActivity extends Activity {

    /** Имена extra совпадают с теми, что кладёт MainActivity при запуске плитки. */
    public static final String EXTRA_IMAGE_URI = "openxiso.imageUri";
    public static final String EXTRA_OS_NAME = "openxiso.osName";

    private static final int SLICE_INSTRUCTIONS = 20000;
    private static final int FRAME_DELAY_MS = 16;

    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Object nativeLock = new Object();

    private long machine;
    private ParcelFileDescriptor descriptor;
    private File cachedImage;
    private Thread cpuThread;
    private volatile boolean active;
    private volatile boolean paused;

    private TextView screenView;
    private TextView statusView;
    private TextView logView;
    private EditText keyInput;
    private Button pauseButton;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_emulator);

        final TextView title = findViewById(R.id.emulatorTitle);
        final TextView firmware = findViewById(R.id.emulatorFirmware);
        screenView = findViewById(R.id.emulatorScreen);
        statusView = findViewById(R.id.emulatorStatus);
        logView = findViewById(R.id.emulatorLog);
        keyInput = findViewById(R.id.emulatorKeyInput);
        pauseButton = findViewById(R.id.emulatorPauseButton);
        final Button sendButton = findViewById(R.id.emulatorSendButton);
        final Button stopButton = findViewById(R.id.emulatorStopButton);

        final String osName = getIntent().getStringExtra(EXTRA_OS_NAME);
        title.setText(getString(R.string.emulator_title)
                + (osName == null || osName.isEmpty() ? "" : " · " + osName));
        firmware.setText(getString(R.string.emulator_subtitle)
                + " · " + NativeBridge.emulatorFirmware());

        sendButton.setOnClickListener(view -> sendTypedCharacters());
        pauseButton.setOnClickListener(view -> togglePause());
        stopButton.setOnClickListener(view -> stopEmulation());

        if (!openImageFromSaf()) {
            return;
        }
        startCpuThread();
    }

    /* ---------------- SAF: Java открывает образ, C читает через fd ---------------- */

    private boolean openImageFromSaf() {
        final String rawUri = getIntent().getStringExtra(EXTRA_IMAGE_URI);
        if (rawUri == null || rawUri.isEmpty()) {
            showFatalError(getString(R.string.emulator_error_open));
            return false;
        }
        final Uri uri = Uri.parse(rawUri);
        descriptor = prepareImageDescriptor(uri);
        if (descriptor == null) {
            showFatalError(getString(R.string.emulator_error_open));
            return false;
        }

        machine = NativeBridge.emulatorCreate();
        if (machine == 0L) {
            showFatalError(getString(R.string.emulator_error_engine));
            return false;
        }

        long size = descriptor.getStatSize();
        if (size < 0L) {
            size = resolveSize(uri);
        }
        if (NativeBridge.emulatorLoadImage(machine, descriptor.getFd(), size) != 0) {
            showFatalError(getString(R.string.emulator_error_image,
                    safeText(NativeBridge.emulatorError(machine))));
            return false;
        }
        if (NativeBridge.emulatorStart(machine) != 0) {
            showFatalError(getString(R.string.emulator_error_start));
            return false;
        }
        return true;
    }

    /*
     * C-ядро читает образ через pread, поэтому нужен позиционируемый дескриптор.
     * Локальные провайдеры SAF его дают сразу; облачные нередко возвращают
     * неseekable pipe — тогда документ копируется в кэш приложения и открывается
     * уже как обычный файл. Кэш удаляется в onDestroy.
     */
    private ParcelFileDescriptor prepareImageDescriptor(Uri uri) {
        ParcelFileDescriptor direct = null;
        try {
            direct = getContentResolver().openFileDescriptor(uri, "r");
        } catch (IOException | RuntimeException ignored) {
            // Переходим к запасному пути через кэш.
        }
        if (direct != null) {
            try {
                Os.lseek(direct.getFileDescriptor(), 0L, OsConstants.SEEK_CUR);
                return direct;
            } catch (ErrnoException notSeekable) {
                try {
                    direct.close();
                } catch (IOException ignored) {
                    // Дескриптор всё равно будет заменён на файловый из кэша.
                }
            }
        }

        final File target = new File(getCacheDir(), "openxiso-boot-image.bin");
        try (InputStream input = getContentResolver().openInputStream(uri);
             OutputStream output = new FileOutputStream(target)) {
            if (input == null) {
                return null;
            }
            final byte[] buffer = new byte[1 << 16];
            int read;
            while ((read = input.read(buffer)) > 0) {
                output.write(buffer, 0, read);
            }
            cachedImage = target;
            return ParcelFileDescriptor.open(target,
                    ParcelFileDescriptor.MODE_READ_ONLY);
        } catch (IOException | RuntimeException error) {
            return null;
        }
    }

    private long resolveSize(Uri uri) {
        final Cursor cursor = getContentResolver().query(uri, null, null, null, null);
        if (cursor == null) {
            return 0L;
        }
        try {
            final int index = cursor.getColumnIndex(OpenableColumns.SIZE);
            if (index >= 0 && cursor.moveToFirst() && !cursor.isNull(index)) {
                return cursor.getLong(index);
            }
        } finally {
            cursor.close();
        }
        return 0L;
    }

    private void showFatalError(String message) {
        statusView.setText(message);
        logView.setText(message);
        screenView.setText("");
    }

    private static String safeText(String value) {
        return value == null ? "" : value;
    }

    /* ---------------- Цикл исполнения x86 в фоновом потоке ---------------- */

    private void startCpuThread() {
        active = true;
        cpuThread = new Thread(() -> {
            while (active) {
                int status;
                synchronized (nativeLock) {
                    if (!paused) {
                        NativeBridge.emulatorRunSlice(machine, SLICE_INSTRUCTIONS);
                    }
                    status = NativeBridge.emulatorStatus(machine);
                }
                if (status == NativeBridge.EMULATOR_HALTED
                        || status == NativeBridge.EMULATOR_ERROR) {
                    active = false;
                }
                mainHandler.post(this::renderFrame);
                try {
                    // В ожидании клавиши INT 16h не дёргаем экран 60 раз в секунду.
                    Thread.sleep(status == NativeBridge.EMULATOR_WAITING
                            ? 80L : FRAME_DELAY_MS);
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    active = false;
                }
            }
            mainHandler.post(this::renderFrame);
        }, "OpenXiso-x86");
        cpuThread.start();
    }

    /** Вызывается только в главном потоке; все обращения к C под одним замком. */
    private void renderFrame() {
        if (machine == 0L) {
            return;
        }
        final String screen;
        final String log;
        final String error;
        final int status;
        final long instructions;
        synchronized (nativeLock) {
            screen = safeText(NativeBridge.emulatorScreen(machine));
            log = safeText(NativeBridge.emulatorLog(machine));
            error = safeText(NativeBridge.emulatorError(machine));
            status = NativeBridge.emulatorStatus(machine);
            instructions = NativeBridge.emulatorInstructions(machine);
        }
        screenView.setText(screen);
        logView.setText(error.isEmpty() ? log : log + "\n" + error);
        statusView.setText(getString(R.string.emulator_state,
                statusLabel(status), instructions));
        pauseButton.setEnabled(status == NativeBridge.EMULATOR_RUNNING
                || status == NativeBridge.EMULATOR_WAITING);
    }

    private String statusLabel(int status) {
        switch (status) {
            case NativeBridge.EMULATOR_RUNNING:
                return getString(R.string.emulator_state_running);
            case NativeBridge.EMULATOR_HALTED:
                return getString(R.string.emulator_state_halted);
            case NativeBridge.EMULATOR_ERROR:
                return getString(R.string.emulator_state_error);
            case NativeBridge.EMULATOR_WAITING:
                return getString(R.string.emulator_state_waiting);
            default:
                return getString(R.string.emulator_state_idle);
        }
    }

    /* ---------------- Клавиатура INT 16h ---------------- */

    /** Младший байт — ASCII в AL, старший — PC-скан-код в AH. */
    private void pushCharacter(char character) {
        if (machine == 0L) {
            return;
        }
        int ascii;
        int scan;
        if (character == '\n' || character == '\r') {
            ascii = 0x0D;
            scan = 0x1C;
        } else if (character == '\b' || character == 0x7F) {
            ascii = 0x08;
            scan = 0x0E;
        } else if (character == '\t') {
            ascii = 0x09;
            scan = 0x0F;
        } else if (character >= 0x20 && character < 0x7F) {
            ascii = character;
            scan = 0;
        } else {
            return; // BIOS INT 16h принимает однобайтовую кодировку загрузчика
        }
        synchronized (nativeLock) {
            NativeBridge.emulatorPushKey(machine, (scan << 8) | ascii);
        }
    }

    private void sendTypedCharacters() {
        final String text = keyInput.getText().toString();
        for (int index = 0; index < text.length(); ++index) {
            pushCharacter(text.charAt(index));
        }
        keyInput.setText("");
        renderFrame();
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (machine != 0L && event != null) {
            final int character = event.getUnicodeChar();
            if (character > 0 && character < 0x80) {
                pushCharacter((char) character);
                return true;
            }
            if (keyCode == KeyEvent.KEYCODE_ENTER) {
                pushCharacter('\n');
                return true;
            }
            if (keyCode == KeyEvent.KEYCODE_DEL) {
                pushCharacter('\b');
                return true;
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    /* ---------------- Управление ---------------- */

    private void togglePause() {
        paused = !paused;
        pauseButton.setText(getString(paused
                ? R.string.emulator_action_resume
                : R.string.emulator_action_pause));
    }

    private void stopEmulation() {
        if (machine == 0L) {
            return;
        }
        synchronized (nativeLock) {
            NativeBridge.emulatorStop(machine);
        }
        active = false;
        renderFrame();
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        active = false;
        if (cpuThread != null) {
            cpuThread.interrupt();
            try {
                cpuThread.join(1000L);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
            }
        }
        mainHandler.removeCallbacksAndMessages(null);
        synchronized (nativeLock) {
            if (machine != 0L) {
                NativeBridge.emulatorDestroy(machine);
                machine = 0L;
            }
        }
        if (descriptor != null) {
            try {
                descriptor.close();
            } catch (IOException ignored) {
                // Дескриптор уже закрыт системой; повторная попытка не нужна.
            }
            descriptor = null;
        }
        if (cachedImage != null) {
            // Кэш нужен был только на время эмуляции.
            cachedImage.delete();
            cachedImage = null;
        }
    }

    /** Вспомогательный метод для MainActivity: удобная сборка Intent запуска. */
    static Intent launchIntent(android.content.Context context, String osName, String imageUri) {
        final Intent intent = new Intent(context, EmulatorActivity.class);
        intent.putExtra(EXTRA_OS_NAME, osName);
        intent.putExtra(EXTRA_IMAGE_URI, imageUri);
        return intent;
    }
}
