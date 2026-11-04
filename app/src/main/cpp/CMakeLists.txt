# Сборка нативной библиотеки libopenxiso.so средствами NDK + CMake.
# Вызывается автоматически из app/build.gradle.kts (externalNativeBuild).
cmake_minimum_required(VERSION 3.22.1)

project(openxiso LANGUAGES C CXX)

# Ядро — чистый C11, мост JNI — C++17.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Эмулятор реального режима, разбор El Torito/MBR, виртуальный PC и свой BIOS.
add_library(openxiso SHARED
        openxiso_core.c
        x86_cpu.c
        boot_image.c
        legacy_bios.c
        pc_machine.c
        openxiso_jni.cpp
        emulator_jni.cpp)

target_include_directories(openxiso PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

# Журнал Android используется только в JNI-мосте для русскоязычных сообщений.
find_library(ANDROID_LOG_LIBRARY log)
target_link_libraries(openxiso ${ANDROID_LOG_LIBRARY})
