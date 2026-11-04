#ifndef OPENXISO_LEGACY_BIOS_H
#define OPENXISO_LEGACY_BIOS_H

/*
 * legacy_bios.h — собственный «маленький Legacy BIOS» OpenXiso.
 * Не SeaBIOS и не чужая прошивка: ROM, IVT, POST и обработчики INT написаны
 * здесь. Каждый вектор IVT ведёт на реальный машинный код-заглушку в F000,
 * который через порт 0x510 вызывает реализацию сервиса на C. Исполняемый
 * путь использует firmware-хук CPU: прошивка получает INT до кадра iret и
 * потому может честно выставлять CF и не портить AL загрузчика.
 */

#include <stdint.h>

#include "pc_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Подпись прошивки для заголовка консоли. */
const char* legacy_bios_version(void);

void legacy_bios_install(PcMachine* machine);
void legacy_bios_post(PcMachine* machine);
void legacy_bios_interrupt(PcMachine* machine, uint8_t vector);
void legacy_bios_log(PcMachine* machine, const char* format, ...);
void legacy_bios_print(PcMachine* machine, const char* utf8, uint8_t attribute);

#ifdef __cplusplus
}
#endif

#endif /* OPENXISO_LEGACY_BIOS_H */
