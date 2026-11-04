/*
 * test_core.c — хостовые тесты C-ядра (обычный gcc, Android SDK/NDK не нужен).
 * Запуск: make -C app/src/main/cpp/tests
 * Проверяет цикл загрузки, валидацию файла, создание конфигурации и разбор
 * синтетического образа ISO 9660 через файловый дескриптор.
 */

#include "openxiso_core.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition, message)                                            \
    do {                                                                     \
        ++checks;                                                            \
        if (!(condition)) {                                                   \
            ++failures;                                                       \
            printf("ОШИБКА: %s (строка %d)\n", (message), __LINE__);          \
        }                                                                     \
    } while (0)

/** Записывает синтетический ISO 9660 с первичным дескриптором в секторе 16. */
static int write_iso_fixture(const char* path) {
    unsigned char image[17 * 2048];
    memset(image, 0, sizeof(image));
    unsigned char* descriptor = image + 16 * 2048;
    descriptor[0] = 1;
    memcpy(descriptor + 1, "CD001", 5);
    descriptor[6] = 1;
    memcpy(descriptor + 40, "OPENXISO TEST DISC", 18);
    const uint32_t sectors = 100;
    descriptor[80] = (unsigned char)(sectors & 0xFFu);
    descriptor[81] = (unsigned char)((sectors >> 8) & 0xFFu);
    descriptor[82] = (unsigned char)((sectors >> 16) & 0xFFu);
    descriptor[83] = (unsigned char)((sectors >> 24) & 0xFFu);

    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    const size_t written = fwrite(image, 1, sizeof(image), file);
    fclose(file);
    return written == sizeof(image);
}

/** Записывает сырой образ .img с сигнатурой загрузочной записи 0x55AA. */
static int write_img_fixture(const char* path) {
    unsigned char image[1024];
    memset(image, 0, sizeof(image));
    image[510] = 0x55;
    image[511] = 0xAA;
    FILE* file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    const size_t written = fwrite(image, 1, sizeof(image), file);
    fclose(file);
    return written == sizeof(image);
}

int main(void) {
    OpenXisoEngine* engine = openxiso_engine_create();
    CHECK(engine != NULL, "движок должен создаваться");

    /* Цикл инициализации. */
    CHECK(openxiso_boot_step_duration_ms() == 2500LL, "шаг загрузки равен 2,5 с");
    CHECK(openxiso_boot_total_steps() == 5, "по умолчанию пять сообщений");
    openxiso_boot_enter_step(engine, 0);
    CHECK(strcmp(openxiso_boot_message(engine), "Загрузка архитектурных библиотек...") == 0,
          "первое сообщение на русском языке");
    openxiso_boot_enter_step(engine, 2);
    CHECK(strcmp(openxiso_boot_message(engine), "Картирование страничной памяти MMU...") == 0,
          "сообщение картирования памяти");
    openxiso_boot_enter_step(engine, 5);
    CHECK(openxiso_boot_stage(engine) == OPENXISO_STAGE_ARCHITECTURE,
          "стадии замыкаются по кругу");
    CHECK(!openxiso_boot_is_complete(engine), "до завершения флаг сброшен");
    openxiso_boot_complete(engine);
    CHECK(openxiso_boot_is_complete(engine), "флаг готовности выставлен явно");

    /* Валидация формы. */
    CHECK(!openxiso_draft_can_submit(engine), "пустая форма не отправляется");
    openxiso_draft_set_os_name(engine, "  Ubuntu 24.04  ");
    CHECK(strcmp(openxiso_draft_os_name(engine), "Ubuntu 24.04") == 0,
          "пробелы в названии обрезаются");
    CHECK(openxiso_draft_select_image(engine, "content://doc/readme.txt", "readme.txt")
          == OPENXISO_ERR_UNSUPPORTED_IMAGE, "расширение .txt отклоняется");
    CHECK(strstr(openxiso_status_message(OPENXISO_ERR_UNSUPPORTED_IMAGE), ".iso") != NULL,
          "текст ошибки на русском и содержит подсказку");
    CHECK(openxiso_draft_select_image(engine, "content://doc/disk.iso", NULL)
          == OPENXISO_ERR_MISSING_FILENAME, "отсутствие имени обрабатывается");
    CHECK(openxiso_draft_select_image(engine, "content://doc/disk.iso", "DISK.ISO")
          == OPENXISO_OK, "регистр расширения не важен");
    CHECK(openxiso_draft_can_submit(engine), "заполненная форма готова к отправке");

    /* Чтение образа через дескриптор. */
    const char* iso_path = "/tmp/openxiso_host_test.iso";
    const char* img_path = "/tmp/openxiso_host_test.img";
    char description[OPENXISO_INFO_CAPACITY];
    if (write_iso_fixture(iso_path)) {
        const int fd = open(iso_path, O_RDONLY);
        CHECK(fd >= 0, "синтетический ISO открыт");
        if (fd >= 0) {
            CHECK(openxiso_probe_image(engine, fd, 17L * 2048L,
                                       description, sizeof(description)) == OPENXISO_OK,
                  "первичный дескриптор тома прочитан");
            CHECK(strstr(description, "ISO 9660") != NULL, "тип образа определён");
            CHECK(strstr(description, "OPENXISO TEST DISC") != NULL,
                  "метка тома извлечена");
            close(fd);
        }
        remove(iso_path);
    } else {
        CHECK(0, "не удалось создать ISO-фикстуру");
    }

    if (write_img_fixture(img_path)) {
        const int fd = open(img_path, O_RDONLY);
        if (fd >= 0) {
            CHECK(openxiso_probe_image(engine, fd, 1024,
                                       description, sizeof(description)) == OPENXISO_OK,
                  "сырой образ прочитан");
            CHECK(strstr(description, "загрузочной записью") != NULL,
                  "сигнатура 0x55AA распознана");
            close(fd);
        }
        remove(img_path);
    } else {
        CHECK(0, "не удалось создать IMG-фикстуру");
    }

    /* Создание конфигурации и сброс черновика. */
    const long long id = openxiso_create_configuration(engine);
    CHECK(id == 1, "первая конфигурация получает идентификатор 1");
    CHECK(openxiso_configuration_count(engine) == 1, "в сетке одна плитка");
    CHECK(strcmp(openxiso_configuration_os_name(engine, 0), "Ubuntu 24.04") == 0,
          "название системы сохранено");
    CHECK(strcmp(openxiso_configuration_image_name(engine, 0), "DISK.ISO") == 0,
          "имя файла сохранено");
    CHECK(!openxiso_draft_can_submit(engine), "черновик очищен после создания");
    CHECK(openxiso_create_configuration(engine) == -1,
          "повторная отправка пустой формы запрещена");

    openxiso_engine_destroy(engine);
    printf("Проверок: %d, ошибок: %d\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
