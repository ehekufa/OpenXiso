// Корневой сценарий сборки: версия AGP задаётся один раз в каталоге версий.
// Плагин Kotlin и Compose удалены: проект теперь Java + C/C++ (JNI).
plugins {
    alias(libs.plugins.android.application) apply false
}
