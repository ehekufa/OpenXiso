// Сборка модуля: интерфейс на Java, нативная библиотека через NDK + CMake.
// Внешних зависимостей нет: используются только платформенные Android API.
plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.openxiso.app"
    compileSdk = 35

    // Фиксируем NDK: та же версия ставится в GitHub Actions и в scripts/build.sh.
    ndkVersion = "27.0.12077973"

    defaultConfig {
        applicationId = "com.openxiso.app"
        minSdk = 26
        targetSdk = 35
        versionCode = 1
        versionName = "1.0.0"

        // Параметры нативной сборки: libopenxiso.so (C11 + C++17).
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += "-DANDROID_STL=c++_static"
            }
        }
        ndk {
            abiFilters += "arm64-v8a"
            abiFilters += "armeabi-v7a"
            abiFilters += "x86_64"
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

dependencies {
    // Пусто намеренно: View, SAF, Handler и JNI входят в платформу Android.
}
