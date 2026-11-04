#!/usr/bin/env bash
#
# Локальная сборка APK «не только из песочницы»: скрипт работает на любой
# машине с JDK 17 и Android SDK. Логика та же, что в .github/workflows/build-apk.yml,
# поэтому результат локальной сборки и CI совпадает.
#
#   export ANDROID_HOME=/путь/к/Android/Sdk
#   ./scripts/build.sh              # отладочный APK
#   ./scripts/build.sh release      # release APK
#   INSTALL_SDK_COMPONENTS=0 ./scripts/build.sh   # не трогать sdkmanager
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VARIANT="${1:-debug}"
case "$VARIANT" in
    debug)   TASK=":app:assembleDebug" ;;
    release) TASK=":app:assembleRelease" ;;
    *) echo "Неизвестный вариант: $VARIANT. Используйте debug или release." >&2; exit 1 ;;
esac

ANDROID_SDK="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}"
if [[ -z "$ANDROID_SDK" ]]; then
    echo "Задайте ANDROID_HOME (или ANDROID_SDK_ROOT) с путём к Android SDK." >&2
    exit 1
fi
if ! command -v java > /dev/null 2>&1; then
    echo "Не найден java: установите JDK 17 и добавьте его в PATH." >&2
    exit 1
fi

# При необходимости ставим те же компоненты, что и workflow.
# Путь к sdkmanager различается у разных версий cmdline-tools — ищем все варианты.
SDKMANAGER=""
for candidate in \
    "$ANDROID_SDK/cmdline-tools/latest/bin/sdkmanager" \
    "$ANDROID_SDK/cmdline-tools/16.0/bin/sdkmanager" \
    "$ANDROID_SDK/cmdline-tools/tools/bin/sdkmanager" \
    "$ANDROID_SDK/tools/bin/sdkmanager"; do
    if [[ -x "$candidate" ]]; then SDKMANAGER="$candidate"; break; fi
done
if [[ "${INSTALL_SDK_COMPONENTS:-1}" == "1" && -n "$SDKMANAGER" ]]; then
    echo "Проверка компонентов Android SDK..."
    yes | "$SDKMANAGER" --sdk_root="$ANDROID_SDK" --licenses > /dev/null 2>&1 || true
    "$SDKMANAGER" --sdk_root="$ANDROID_SDK" --install \
        "platforms;android-35" \
        "build-tools;34.0.0" \
        "build-tools;35.0.0" \
        "platform-tools" \
        "ndk;27.0.12077973" \
        "cmake;3.22.1" > /dev/null
fi

echo "Запуск хостовых тестов C-ядра и реальной загрузки El Torito/MBR..."
make -C app/src/main/cpp/tests run

chmod +x gradlew
echo "Сборка APK ($VARIANT)..."
./gradlew "$TASK" --stacktrace --no-daemon

if [[ "$VARIANT" == "debug" ]]; then
    APK="app/build/outputs/apk/debug/app-debug.apk"
else
    APK="app/build/outputs/apk/release/app-release-unsigned.apk"
fi

echo "Готово: $ROOT/$APK"
ls -lh "$APK"
