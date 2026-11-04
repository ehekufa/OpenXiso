# OpenXiso

[![Сборка APK OpenXiso](https://github.com/ehekufa/OpenXiso/actions/workflows/build-apk.yml/badge.svg)](https://github.com/ehekufa/OpenXiso/actions/workflows/build-apk.yml)

Премиальная утилита виртуализации для Android: сине-белый интерфейс в стиле
Google Material 3. Теперь проект написан **на Java + C + C++ вместе**:

* **Java** — весь интерфейс и **загрузка файлов образа из памяти телефона**
  (`ACTION_OPEN_DOCUMENT`, `ContentResolver`, `OpenableColumns`,
  `ParcelFileDescriptor`, передача файлового дескриптора в нативный код);
* **C (C11)** — ядро: состояние, флаги, цикл инициализации, валидация, список
  конфигураций и чтение образа (ISO 9660 / сигнатура `0x55AA`);
* **C++ (C++17)** — только тонкий мост JNI без бизнес-логики.

Kotlin и Jetpack Compose из проекта удалены. Все комментарии, сообщения журнала,
тексты ошибок и строки интерфейса — на русском языке; идентификаторы — на английском.

---

## ГДЕ ВСЁ БУДЕТ (карта файлов)

Тот же комментарий продублирован в шапке `MainActivity.java` и в `openxiso_core.h`.

```
OpenXiso/
├── README.md                       ← эта инструкция
├── .github/workflows/build-apk.yml  ← CI: автосборка APK на GitHub Actions
├── scripts/build.sh                ← тот же билд локально на любой машине
├── settings.gradle.kts             ← репозитории и модуль :app
├── build.gradle.kts                ← корневой сценарий (только AGP)
├── gradle/libs.versions.toml       ← версия Android Gradle Plugin
├── gradle/wrapper/, gradlew        ← Gradle Wrapper 8.9 (уже лежит в репозитории)
└── app/
    ├── build.gradle.kts            ← externalNativeBuild → CMake, ABI, Java 17
    ├── proguard-rules.pro
    └── src/main/
        ├── AndroidManifest.xml
        ├── java/com/openxiso/app/
        │   ├── MainActivity.java       ← ИНТЕРФЕЙС + SAF + Handler-цикл загрузки
        │   ├── EmulatorUiState.java    ← UI-флаги, Bundle, синхронизация с ядром
        │   ├── NativeBridge.java       ← native-методы и владелец движка
        │   └── UiKit.java              ← фабрика View, форм, обводок и ripple
        ├── cpp/
        │   ├── CMakeLists.txt          ← сборка libopenxiso.so
        │   ├── include/openxiso_core.h ← публичный C-API ядра
        │   ├── openxiso_core.c         ← ЯДРО: состояние, цикл, ISO 9660
        │   ├── openxiso_jni.cpp        ← МОСТ JNI (C++17)
        │   └── tests/
        │       ├── test_core.c         ← хостовые тесты ядра на gcc
        │       └── Makefile
        └── res/
            ├── layout/activity_main.xml          ← загрузчик + рабочее пространство
            ├── layout/dialog_configuration.xml  ← модальное окно 28 dp
            ├── drawable/                        ← «плюс», ISO-документ, bg_dialog,
            │                                     bg_file_box (пунктир), bg_brand_mark,
            │                                     bg_button
            ├── values/{strings,colors,themes}.xml
            ├── values-v27/themes.xml
            ├── xml/                             ← правила резервного копирования
            └── mipmap-anydpi-v26/              ← адаптивные значки
preview/index.html                  ← справочный HTML-макет дизайна (в APK не входит)
```

**Куда что положить при сборке:** просто откройте корень `OpenXiso/` в Android
Studio — файлы уже на своих местах, дополнительно ничего переносить не нужно.
Нативная библиотека собирается автоматически (`externalNativeBuild`) и попадает
в APK как `lib/<abi>/libopenxiso.so`.

---

## CI: APK собирается сам через workflow

Основной способ получения APK — **`.github/workflows/build-apk.yml`**. Сборка идёт
на раннере GitHub, а не в песочнице агента: JDK 17, Android SDK 35,
NDK `27.0.12077973`, CMake `3.22.1`, затем `assembleDebug` и `assembleRelease`.

**Когда запускается:**

* `push` в `arena/47099952-openxiso` или `main` (кроме правок только в `*.md` / `preview/**`);
* `pull_request` в `main`;
* пуш тега `v*` — дополнительно создаёт GitHub Release с прикреплёнными APK;
* вручную: `Actions → Сборка APK OpenXiso → Run workflow` (можно выбрать
  `both` / `debug` / `release`), либо `gh workflow run build-apk.yml --ref arena/47099952-openxiso`.

**Где забрать APK:** откройте нужный запуск во вкладке **Actions** → блок
**Artifacts** → `openxiso-debug-apk` / `openxiso-release-apk` (хранятся 30 дней).
Скачать из CLI на своей машине: `gh run download <run-id> -n openxiso-debug-apk`.
Отдельный job `native-tests` перед сборкой гоняет тесты C-ядра на обычном gcc.

**Проверенный зелёный запуск:** [41779458771](https://github.com/ehekufa/OpenXiso/actions/runs/41779458771)
— собраны `app-debug.apk` (846 КБ) и `app-release-unsigned.apk` (711 КБ).
Шаг «Проверка состава APK» подтвердил `classes.dex`, `AndroidManifest.xml`,
`resources.arsc` и `lib/<abi>/libopenxiso.so` для `arm64-v8a`, `armeabi-v7a`,
`x86_64`; итоговая таблица пишется в Job Summary. Прямые ссылки на артефакты:
[debug](https://github.com/ehekufa/OpenXiso/actions/runs/41779458771/artifacts/13298776000),
[release](https://github.com/ehekufa/OpenXiso/actions/runs/41779458771/artifacts/13297723875).

> Из песочницы агента артефакт скачать нельзя: хост blob-хранилища GitHub закрыт
> сетевыми ограничениями, а токен не имеет права `actions: write` для ручного
> `gh workflow run` (403). В браузере и на вашей машине оба ограничения отсутствуют:
> кнопка **Run workflow** и скачивание APK работают. Push на ветку сборку запускает
> всегда — именно так она и была проверена.

---

## Как собрать APK у себя (не только из песочницы)

В самой песочнице JDK/Android SDK/NDK нет, поэтому сборка вынесена в CI. На любой
машине с Android SDK можно собрать тем же набором шагов:

```bash
export ANDROID_HOME=/путь/к/Android/Sdk
./scripts/build.sh            # debug
./scripts/build.sh release    # release
```

Скрипт проверит компоненты SDK, запустит хостовые тесты ядра и выведет путь к APK.
Либо вручную:

1. Android Studio Ladybug или новее, **JDK 17**, Android SDK 35.
2. В SDK Manager установите **NDK `27.0.12077973`** и **CMake 3.22.1**.
3. Откройте папку `OpenXiso` как проект Gradle.
4. Соберите: `./gradlew :app:assembleDebug` (или `Build → Make Project`).

**Готовый APK:** `app/build/outputs/apk/debug/app-debug.apk`
Release: `./gradlew :app:assembleRelease` → `app/build/outputs/apk/release/app-release-unsigned.apk`.

Хостовые тесты C-ядра не требуют Android SDK и уже проверены в песочнице:

```bash
make -C app/src/main/cpp/tests run
# Проверок: 27, ошибок: 0
```

---

## Что где работает

| Требование | Реализация |
| --- | --- |
| Загрузчик с индикатором `#1A73E8` и текстом через 24 dp | `activity_main.xml` + `MainActivity.bootStepRunnable` |
| Сообщения каждые 2,5 с, циклически, на русском | `openxiso_core.c` (`kBootMessages`) + `Handler.postDelayed` |
| Явное завершение инициализации → сетка | `openxiso_boot_complete()` / `completeBoot()`, `showWorkspace()` |
| Плитка «плюс» 120×120 dp, рамка 1 dp, радиус 16 dp, ripple | `UiKit.createPlusTile()` + `R.drawable.ic_plus` |
| Модальное окно, радиус 28 dp | `bg_dialog.xml` + `dialog_configuration.xml` |
| Плоское поле с анимацией рамки в Google Blue | `MainActivity.animateFieldFocus()` (`ValueAnimator` + `ArgbEvaluator`) |
| Ярлык «Название операционной системы» | `strings.xml` → `label_os_name` |
| Контейнер файла: пунктир, 24 dp, документ, подсказка | `bg_file_box.xml` + `dialog_configuration.xml` |
| Выбор `.iso` / `.img` с телефона (SAF) | `MainActivity.openDocumentPicker()` / `onActivityResult()` |
| Чтение выбранного файла | Java открывает `ParcelFileDescriptor`, C читает `fd` через `pread` |
| Проверка расширения и тексты ошибок | `openxiso_draft_select_image()` → `statusMessage()` |

### Путь файла от телефона до C

```
ACTION_OPEN_DOCUMENT → Uri → OpenableColumns.DISPLAY_NAME
    → NativeBridge.selectImage(uri, имя)        [проверка .iso/.img в C]
    → ContentResolver.openFileDescriptor(uri,"r")
    → ParcelFileDescriptor.getFd() / getStatSize()
    → NativeBridge.probeImage(fd, размер)       [C: ISO 9660, сектор 16, «CD001»]
    → описание образа на русском показывается в диалоге
```

---

## Важное решение по «20-летнему» загрузчику

Спецификация одновременно требует «бесконечную» загрузку до 20 лет **и** явное
завершение инициализации с показом рабочего пространства. Буквальный бесконечный
цикл навсегда скрыл бы плитку, диалог и выбор файла. Поэтому архитектура
20-летнего цикла реализована полностью, но по умолчанию выключена:

* `OPENXISO_MAX_BOOT_YEARS = 20`, бюджет `20 * 365 * 24 * 60 * 60 * 1000` мс;
* при `OPENXISO_ENDLESS_BOOT_SIMULATION = 1` в `openxiso_core.h`
  `openxiso_boot_total_steps()` вернёт **252 288 000** шагов по 2,5 с — ровно
  20 лет циклического обновления сообщений;
* по умолчанию `OPENXISO_ENDLESS_BOOT_SIMULATION = 0`: один цикл из пяти
  служебных сообщений (12,5 с), затем `completeBoot()` и рабочее пространство.

Цикл построен на `Handler.postDelayed` (не блокирует главный поток) и продолжается с
сохранённого шага после поворота экрана: индекс шага лежит в `EmulatorUiState`
и восстанавливается через `Bundle`.

> Примечание: Google Sans / Product Sans — проприетарные шрифты и не входят в
> AOSP, поэтому используется системный `sans-serif-medium`. При наличии
> лицензии подставьте свой `Typeface` в `MainActivity.renderBootStep()` и
> `UiKit.createConfigurationTile()`.

---

## Совместимость

* `minSdk 26`, `targetSdk 35`, `compileSdk 35`;
* ABI: `arm64-v8a`, `armeabi-v7a`, `x86_64`;
* внешних Maven-зависимостей нет — только платформенные Android API;
* `startActivityForResult` выбран сознательно (проект без AndroidX). При
  добавлении `androidx.activity:activity` его можно заменить на
  `registerForActivityResult(new ActivityResultContracts.OpenDocument(), ...)`.

`preview/index.html` — автономный макет трёх состояний для визуального ревью,
не является частью Android-сборки.
