# OpenXiso

[![Сборка APK OpenXiso](https://github.com/ehekufa/OpenXiso/actions/workflows/build-apk.yml/badge.svg)](https://github.com/ehekufa/OpenXiso/actions/workflows/build-apk.yml)

Премиальная утилита виртуализации для Android: сине-белый интерфейс в стиле
Google Material 3. Теперь проект написан **на Java + C + C++ вместе**:

* **Java** — весь интерфейс и **загрузка файлов образа из памяти телефона**
  (`ACTION_OPEN_DOCUMENT`, `ContentResolver`, `OpenableColumns`,
  `ParcelFileDescriptor`, передача файлового дескриптора в нативный код);
* **C (C11)** — ядро: состояние, флаги, цикл инициализации, валидация, список
  конфигураций, чтение образа, **интерпретатор x86 в трёх разрядностях**
  (16-битный реальный режим, 32-битный protected mode, 64-битный long mode),
  разбор El Torito / MBR, виртуальный PC (VGA 80×25, порты, клавиатура) и
  **собственный маленький Legacy BIOS** (ROM, IVT, POST, сервисы INT);
* **C++ (C++17)** — только тонкий мост JNI без бизнес-логики.

Это **не интерфейс-имитация**: приложение исполняет настоящие машинные коды
загрузочного сектора и ядра. Прошивка написана с нуля — не SeaBIOS и не чужая ROM.
В диалоге создания конфигурации выбирается **16 / 32 / 64 бита**, и этот выбор
действительно меняет набор доступных инструкций процессора.

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
        │   ├── EmulatorActivity.java   ← КОНСОЛЬ эмулятора: VGA-экран и INT 16h
        │   ├── EmulatorUiState.java    ← UI-флаги, Bundle, синхронизация с ядром
        │   ├── NativeBridge.java       ← native-методы и владелец движка
        │   └── UiKit.java              ← фабрика View, форм, обводок и ripple
        ├── cpp/
        │   ├── CMakeLists.txt          ← сборка libopenxiso.so
        │   ├── include/openxiso_core.h ← C-API ядра конфигураций
        │   ├── include/x86_cpu.h       ← C-API интерпретатора 8086/i386/x86-64
        │   ├── include/boot_image.h    ← план загрузки El Torito / MBR
        │   ├── include/pc_machine.h    ← виртуальный PC и экран 80x25
        │   ├── include/legacy_bios.h   ← собственный Legacy BIOS
        │   ├── openxiso_core.c         ← состояние, цикл, ISO 9660, разрядность
        │   ├── x86_cpu.c               ← 16-битные инструкции: ModRM, ALU, INT
        │   ├── x86_wide.c              ← 32/64 бита: REX, GDT/CR0/EFER, SIB, RIP
        │   ├── boot_image.c            ← PVD, каталог El Torito, MBR, pread
        │   ├── legacy_bios.c           ← ROM F0000, IVT, POST, INT 10h/13h/16h/19h
        │   ├── pc_machine.c            ← ОЗУ, VGA CP866→UTF-8, порты, клавиши
        │   ├── openxiso_jni.cpp        ← МОСТ JNI конфигураций (C++17)
        │   ├── emulator_jni.cpp        ← МОСТ JNI виртуального PC (C++17)
        │   └── tests/
        │       ├── test_core.c         ← хостовые тесты ядра на gcc
        │       ├── test_emulator.c     ← El Torito-ISO + MBR + floppy, реальный boot
        │       ├── test_wide.c         ← РЕАЛЬНЫЙ переход в i386 и long mode
        │       └── Makefile
        └── res/
            ├── layout/activity_main.xml          ← загрузчик + рабочее пространство
            ├── layout/activity_emulator.xml     ← консоль настоящего эмулятора
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

## Настоящий эмулятор и свой маленький Legacy BIOS

Реализовано с нуля, без внешних прошивок и без библиотек эмуляции:

| Компонент | Что делает |
| --- | --- |
| `x86_cpu.c` | Интерпретатор реального режима 8086/186: сегменты, 1 МБ плоской памяти, ModRM, префиксы, ALU, флаги, стек, строковые операции, сдвиги, порты ввода-вывода, `INT`, `IRET`, `HLT` |
| `boot_image.c` | Читает через `pread` PVD ISO 9660 (сектор 16), boot record (сектор 17), каталог El Torito, initial/validation entry, либо MBR с `0x55AA`; различает **все пять режимов El Torito** (без эмуляции, дискета 1,2/1,44/2,88 МБ, жёсткий диск) и строит план загрузки |
| `legacy_bios.c` | ROM по `F0000`, 256 настоящих машинных заглушек в IVT, POST `F000:FE00`, вектор сброса `F000:FFF0`, BDA, подпись `OpenXiso Little Legacy BIOS 0.1`, сервисы `INT 10h` (VGA-текст), `13h` (CHS + EDD `41h/42h/48h`), `16h`, `19h`, а также `08h/11h/12h/15h/1Ah/1Ch` |
| `x86_wide.c` | 32- и 64-битное ядро: префиксы `0x66/0x67`, REX (`R8…R15`, 64-битные операнды), ModRM/SIB, RIP-относительная адресация, `LGDT/LIDT`, `MOV CR0/CR3/CR4`, `RDMSR/WRMSR (EFER)`, far jmp по дескриптору GDT с битами D/B и L, CPUID, `MOVZX`, `IMUL`, CALL/RET/Jcc |
| `pc_machine.c` | 1 МБ для 16 бит / 64 МБ для 32–64 бит, VGA 80×25 с переводом CP866→UTF-8, очередь клавиш, COM1, порты `0x60/0x64/0x3F8/0x3FD/0x510/0x511/0xCF9`, срез исполнения, русскоязычный журнал POST |
| `EmulatorActivity.java` | SAF открывает образ и удерживает `ParcelFileDescriptor`; фоновый поток исполняет инструкции, главный поток рисует 25 строк VGA, журнал и счётчик инструкций; клавиши уходят в `INT 16h` |

**Как устроена честная загрузка.** `INT 19h` читает загрузчик по плану
El Torito/MBR в `0000:7C00`, выставляет `DL` по режиму эмуляции и передаёт
управление far-jmp. Firmware-хук CPU получает `INT` до кадра `iret`, поэтому
`CF` от дискового сервиса не затирается восстановлением FLAGS, а `AL` не портится
ROM-заглушкой; сами заглушки остаются реальными машинными кодами в IVT.

**Эмуляция диска — важная деталь.** Байт 1 initial entry каталога El Torito
задаёт тип эмуляции. При **no emulation** гость видит весь CD: `DL=0xE0`,
`INT 13h AH=42h` работает секторами по **2048** байт от начала ISO. При
**эмуляции дискеты/жёсткого диска** BIOS обязан показать гостю не ISO, а только
сам загрузочный образ: `DL=0x00` (или `0x80`), секторы по **512** байт от
`boot_lba*2048`, геометрия CHS берётся из BPB образа. Именно так собран
`dimos.iso` (`xorriso -b dimos.img -c boot.cat` без `-no-emul-boot`): его
загрузчик ищет `KERNEL.BIN` в корневом каталоге FAT12 по LBA 19. Если перепутать
режимы, чтение уходит в 4 раза дальше по файлу и загрузчик закономерно пишет
`No KERNEL.BIN`. Теперь оба режима реализованы, включая резервный CHS-путь
`AH=02h`, `AH=08h` (геометрия), `AH=41h/48h` (EDD) и возврат числа блоков
в DAP. `AH=48h` сообщает честный размер сектора, а журнал сворачивает сотни
одинаковых чтений в одну строку со счётчиком.

## Выбор разрядности: 16 / 32 / 64 бита

В диалоге «Новая конфигурация» появился переключатель **«Разрядность системы»**.
Это не декоративная подпись: выбранный профиль сохраняется в C-ядре вместе с
конфигурацией, показывается на плитке и передаётся в `EmulatorActivity`, где
вызывает `openxiso_machine_set_cpu_mode()` до POST.

| Профиль | Процессор | Память | Что разрешено |
| --- | --- | --- | --- |
| **16 бит** | 8086/186, реальный режим | 1 МБ | Только 16-битные инструкции. Любой `0x66/0x67/0Fh` честно останавливается с русским сообщением |
| **32 бита** | i386, protected mode | 64 МБ | `LGDT`, `CR0.PE`, far jmp по дескриптору с `D/B=1`, 32-битные регистры и ALU |
| **64 бита** | x86-64, long mode | 64 МБ | Всё из i386 плюс `EFER.LME` через `WRMSR`, дескриптор с `L=1`, REX-префиксы, `R8…R15`, 64-битные операнды, `MOVABS`, RIP-адресация |

Машина **всегда стартует в реальном режиме** (`F000:FFF0`), как настоящий PC:
POST, IVT и загрузочный сектор исполняются 16-битным ядром, а гость сам
переключает разрядность. Профиль задаёт «потолок»: попытка 32-битного перехода
в профиле 16 бит или long mode в профиле 32 бита даёт понятную ошибку, а не
тихий сбой. `CPUID` тоже отвечает честно — бит Long Mode (`EDX.29`) выставляется
только в 64-битном профиле.

`test_wide.c` это проверяет на машинных кодах: загрузчик реально выполняет
`LGDT → MOV CR0 → far jmp` и попадает в 32-битный код (`EAX = 0x12345679`),
либо `WRMSR EFER → CR0 → far jmp` и попадает в 64-битный код
(`RAX = 0x12345679`, `R8 = 43`). Третий тест убеждается, что в профиле 16 бит
тот же загрузчик получает отказ.

**Как проверить без телефона:**

```bash
make -C app/src/main/cpp/tests run
# Проверок: 27, ошибок: 0        ← тесты ядра конфигураций
# Состояние: 2, инструкций: 269  ← El Torito: POST → загрузчик → INT 10h → INT 13h → HLT
# Проверок: 28, ошибок: 0        ← MBR, INT 16h, эмуляция дискеты, EDD и CHS
```

`test_emulator.c` сам собирает ISO 9660 без эмуляции, ISO с **эмуляцией дискеты
1,44 МБ** в стиле DimOS, образ с MBR и три 16-битных загрузчика из машинных
кодов. Проверяются баннер BIOS, строки загрузчика, успешное чтение LBA через
`AH=42h` и через CHS `AH=02h`, поиск `KERNEL.BIN` по LBA 19, геометрия 18/2,
останов по `HLT`, ветка MBR, ожидание клавиши `INT 16h` и продолжение после неё.
`test_wide.c` собирает загрузчики, которые реально переключают CPU в i386 и
x86-64. Оба теста в CI дополнительно прогоняются под ASan/UBSan.

**Как запустить в приложении:** создайте конфигурацию через SAF, выберите
разрядность (16 / 32 / 64 бита) и нажмите на плитку — откроется консоль
Legacy BIOS с указанным профилем CPU. Кнопки «Пауза», «Продолжить»,
«Остановить» управляют циклом ЦПУ, поле ввода и аппаратная клавиатура кладут
символы в очередь `INT 16h` (младший байт — ASCII в `AL`, старший — скан-код в `AH`).
Ядро читает образ через `pread`, поэтому Java проверяет дескриптор SAF на
позиционируемость: если провайдер отдал неseekable pipe (облачные диски), документ
временно копируется в кэш приложения и удаляется при закрытии консоли.

> Ограничение честно: это интерпретатор, а не полный PC. Реализованы реальный
> режим, базовый protected mode i386 и базовый long mode x86-64 — но **нет
> страничной памяти (CR3 только хранится), IDT/прерываний PM, колец защиты,
> TSS, SSE/AVX, APIC/ACPI, PCI/IDE-контроллера и графических режимов VBE**.
> Ядро DimOS 14 с VBE-фреймбуфером и драйверами всё равно не запустится:
> после перехода в protected mode ему понадобится аппаратура, которой здесь нет.
> Зато POST, IVT, MBR, все режимы El Torito, FAT12-стадия, сервисы BIOS,
> VGA-текст и реально исполняемые 16/32/64-битные инструкции — настоящие.

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

**Проверенный зелёный запуск с выбором разрядности:** [41815756135](https://github.com/ehekufa/OpenXiso/actions/runs/41815756135)
— собраны `app-debug.apk` (1,3 МБ) и `app-release-unsigned.apk` (1,1 МБ).
Job `native-tests` выполнил 27 проверок ядра, 28 проверок реальной загрузки BIOS
(El Torito, эмуляция дискеты, поиск `KERNEL.BIN`) и 20 проверок настоящих
режимов i386/x86-64, плюс два прогона под ASan/UBSan. Шаг «Проверка состава APK»
подтвердил `classes.dex` (6472 Б), `AndroidManifest.xml` (2576 Б),
`resources.arsc` (9388 Б) и `lib/<abi>/libopenxiso.so` для всех трёх ABI:
`arm64-v8a` 478512 Б, `armeabi-v7a` 287604 Б, `x86_64` 461384 Б.
Контрольные суммы zip-артефактов: debug
`93523719c70de96573f4c09403cf53e8c096f0f0b09330ad81d9c56e8c2ad7b3`, release
`8f2d4420c7fced4db0d8677f9c605cc26ffbb457afef779ee5a7d77c6fce73d1`.
Прямые ссылки:
[debug](https://github.com/ehekufa/OpenXiso/actions/runs/41815756135/artifacts/13314149416),
[release](https://github.com/ehekufa/OpenXiso/actions/runs/41815756135/artifacts/13314368352).

Предыдущий зелёный запуск с исправлением El Torito — [41795897135](https://github.com/ehekufa/OpenXiso/actions/runs/41795897135).

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

Скрипт проверит компоненты SDK, запустит хостовые тесты ядра и реальной загрузки
BIOS, затем выведет путь к APK.
Либо вручную:

1. Android Studio Ladybug или новее, **JDK 17**, Android SDK 35.
2. В SDK Manager установите **NDK `27.0.12077973`** и **CMake 3.22.1**.
3. Откройте папку `OpenXiso` как проект Gradle.
4. Соберите: `./gradlew :app:assembleDebug` (или `Build → Make Project`).

**Готовый APK:** `app/build/outputs/apk/debug/app-debug.apk`
Release: `./gradlew :app:assembleRelease` → `app/build/outputs/apk/release/app-release-unsigned.apk`.

Хостовые тесты C-ядра и эмулятора не требуют Android SDK и уже проверены
в песочнице:

```bash
make -C app/src/main/cpp/tests run
# Проверок: 27, ошибок: 0
# Проверок: 28, ошибок: 0
# 32 бита: режим=1, RAX=12345679
# 64 бита: режим=2, RAX=12345679, R8=43
# Проверок: 20, ошибок: 0
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
| Настоящая загрузка El Torito / MBR | `boot_image.c` + `legacy_bios.c` + `x86_cpu.c` |
| Собственный Legacy BIOS (ROM/IVT/POST/INT) | `legacy_bios.c`, firmware-хук `X86Cpu.interrupt_hook` |
| VGA-экран 80×25 и клавиатура INT 16h | `activity_emulator.xml` + `EmulatorActivity.java` |

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
