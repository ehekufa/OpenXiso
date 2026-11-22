/* Manual, bounded smoke test of a real image, not part of the passing test suite. */
#include "pc_machine.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s path/to/image.img\n", argv[0]);
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        perror(argv[1]);
        if (fd >= 0) close(fd);
        return 2;
    }
    PcMachine* machine = openxiso_machine_create();
    if (!machine) { close(fd); return 2; }
    if (openxiso_machine_set_cpu_mode(machine, 1) != 0 ||
        openxiso_machine_load_image(machine, fd, st.st_size) != 0 ||
        openxiso_machine_start(machine) != 0) {
        fprintf(stderr, "Ошибка: %s\n", openxiso_machine_error(machine));
        openxiso_machine_destroy(machine);
        close(fd);
        return 2;
    }
    for (int i = 0; i < 2000 && openxiso_machine_run_slice(machine, 10000); ++i) {}
    printf("Статус: %d; инструкций: %" PRIu64 "; режим: %d; CS:IP=%04x:%04x\n",
           machine->status, openxiso_machine_instructions(machine), machine->cpu.mode,
           machine->cpu.sregs[X86_CS], machine->cpu.ip);
    for (int row = 0; row < OPENXISO_SCREEN_ROWS; ++row) {
        char line[256];
        if (openxiso_machine_get_screen_line(machine, row, line, sizeof(line)) >= 0)
            puts(line);
    }
    printf("\nЖурнал:\n%s\nОшибка: %s\n", openxiso_machine_log(machine),
           openxiso_machine_error(machine));
    puts("Это диагностический запуск: сам по себе он не подтверждает загрузку Linux.");
    openxiso_machine_destroy(machine);
    close(fd);
    return 0;
}
