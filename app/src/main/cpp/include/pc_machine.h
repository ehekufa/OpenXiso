#ifndef OPENXISO_PC_MACHINE_H
#define OPENXISO_PC_MACHINE_H

#include <stdint.h>
#include "boot_image.h"
#include "x86_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OPENXISO_MEMORY_SIZE (64u * 1024u * 1024u)
#define OPENXISO_WIDE_MEMORY_SIZE (64u * 1024u * 1024u)
#define OPENXISO_VGA_ADDRESS 0xB8000u
#define OPENXISO_BIOS_ROM_ADDRESS 0xF0000u
#define OPENXISO_BOOT_ADDRESS 0x7C00u
#define OPENXISO_SCREEN_COLUMNS 80
#define OPENXISO_SCREEN_ROWS 25
#define OPENXISO_FIRMWARE_PORT 0x0510u
#define OPENXISO_FIRMWARE_POST_PORT 0x0511u
#define OPENXISO_LOG_CAPACITY 32

#define OPENXISO_LFB_BASE 0xE0000000ULL
#define OPENXISO_LFB_SIZE (16u * 1024u * 1024u)
#define OPENXISO_LAPIC_BASE 0xFEE00000ULL
#define OPENXISO_IOAPIC_BASE 0xFEC00000ULL
#define OPENXISO_HPET_BASE 0xFED00000ULL

typedef enum PcMachineStatus {
    OPENXISO_MACHINE_IDLE = 0,
    OPENXISO_MACHINE_RUNNING = 1,
    OPENXISO_MACHINE_HALTED = 2,
    OPENXISO_MACHINE_ERROR = 3,
    OPENXISO_MACHINE_WAITING = 4
} PcMachineStatus;

typedef struct PcMachine {
    X86Cpu cpu;
    uint8_t* memory;
    size_t memory_size;
    X86Mode cpu_mode;
    int fd;
    long long image_size;
    OpenXisoBootPlan plan;
    PcMachineStatus status;
    uint16_t keys[64];
    int key_head;
    int key_tail;
    uint8_t cursor_row;
    uint8_t cursor_col;
    uint32_t ticks;
    char serial_line[192];
    int serial_length;
    char log_lines[OPENXISO_LOG_CAPACITY][192];
    int log_count;
    char log_last[192];
    int log_repeat;
    char log_snapshot[OPENXISO_LOG_CAPACITY * 200];
    char error[192];
    uint8_t transfer_buffer[65536];

    /* --- PIC 8259 --- */
    int pic_base_master;
    int pic_base_slave;
    uint8_t pic_imr_master;
    uint8_t pic_imr_slave;
    uint8_t pic_irr;
    uint8_t pic_isr;
    uint8_t pic_irr_slave;
    uint8_t pic_isr_slave;
    uint8_t pic_icw_master[4];
    uint8_t pic_icw_slave[4];
    int pic_icw_step_master;
    int pic_icw_step_slave;
    uint8_t pic_ocw3_master;
    uint8_t pic_ocw3_slave;
    int pic_single;
    int pic_icw4_needed_master;
    int pic_icw4_needed_slave;

    /* --- PIT 8254 --- */
    uint32_t pit_count[3];
    uint32_t pit_reload[3];
    uint8_t pit_mode[3];
    uint8_t pit_latch[3];
    int pit_latch_pending[3];
    int pit_latch_high[3];
    uint8_t pit_access[3];
    int pit_gate[3];
    uint64_t pit_phase;
    uint8_t pit_read_latch[3];
    int pit_read_pending[3];

    /* --- Keyboard controller 8042 --- */
    uint8_t kbd_command_byte;
    int kbd_expect_command;
    int kbd_expect_mouse;
    uint8_t kbd_output;
    int kbd_output_full;
    int kbd_input_full;
    int kbd_enabled;
    int kbd_mouse_enabled;
    uint8_t kbd_input_buffer;

    /* --- CMOS/RTC --- */
    uint8_t cmos_index;
    uint8_t cmos_memory[128];

    /* --- DMA 8237 --- */
    uint16_t dma_addr[4];
    uint16_t dma_count[4];
    uint8_t dma_page[4];
    uint8_t dma_mode[4];
    uint8_t dma_mask;
    int dma_flipflop;
    uint8_t dma_addr_high[4];
    uint8_t dma_count_high[4];

    /* --- PCI --- */
    uint8_t pci_config[32][8][256];
    uint8_t pci_present[32][8];
    uint32_t pci_addr;
    int pci_addr_valid;

    /* --- IDE --- */
    uint8_t ide_regs[2][8];
    uint8_t ide_status[2];
    uint8_t ide_error[2];
    uint8_t ide_alt_status[2];
    uint8_t ide_device_control[2];
    int ide_drq[2];
    int ide_atapi[2];
    uint16_t ide_identify[2][256];
    uint8_t ide_buffer[65536];
    int ide_buffer_pos;
    int ide_buffer_len;
    uint8_t ide_features[2];
    uint8_t ide_sector_count[2];
    uint8_t ide_lba[2][4];
    uint8_t ide_drive_head[2];
    uint8_t ide_command[2];
    int ide_irq_pending[2];

    /* --- VGA / VBE --- */
    int vga_mode;
    int vga_width;
    int vga_height;
    int vga_bpp;
    int vga_pitch;
    uint32_t vga_lfb_phys;
    int vga_bank;
    int vga_bank_size;
    uint8_t vga_palette[256][3];
    uint8_t vga_seq[32];
    uint8_t vga_crtc[32];
    uint8_t vga_gc[32];
    uint8_t vga_attr[32];
    uint8_t vga_misc;
    uint8_t vga_dac_index;
    uint8_t vga_dac_read_index;
    int vga_dac_state;
    uint8_t* framebuffer;
    size_t framebuffer_size;
    int fb_width;
    int fb_height;
    int fb_bpp;
    int fb_pitch;
    int fb_mode; /* 0=text,1=mode13,2=vbe */
    uint8_t vbe_info[512];
    int vbe_lfb_enabled;

    /* --- APIC / IOAPIC / HPET --- */
    uint32_t lapic_regs[256];
    uint32_t ioapic_regs[256];
    uint32_t hpet_regs[256];
    uint32_t lapic_timer_count;
    uint32_t lapic_timer_init;
    uint8_t lapic_timer_mode;
    uint8_t lapic_timer_vector;
    int lapic_timer_pending;
    uint64_t lapic_phase;
    uint32_t ioapic_redtbl[24][2];
    uint64_t hpet_phase;
    uint32_t hpet_counter_low;
    int hpet_enabled;

    /* --- Floppy --- */
    uint8_t fdc_regs[8];
    uint8_t fdc_status;
    int fdc_drq;
    uint8_t fdc_buffer[512];
    int fdc_buffer_pos;

    /* --- Time --- */
    uint64_t last_time_ns;
    uint64_t emulated_ns;
    int time_initialized;

    /* --- ACPI --- */
    uint32_t acpi_rsdp_addr;
    int acpi_installed;
} PcMachine;

PcMachine* openxiso_machine_create(void);
void openxiso_machine_destroy(PcMachine* machine);
int openxiso_machine_set_cpu_mode(PcMachine* machine, int mode);
X86Mode openxiso_machine_cpu_mode(const PcMachine* machine);
int openxiso_machine_load_image(PcMachine* machine, int fd, long long size);
int openxiso_machine_start(PcMachine* machine);
int openxiso_machine_run_slice(PcMachine* machine, uint32_t max_instructions);
int openxiso_machine_get_screen_line(const PcMachine* machine, int row, char* out, size_t capacity);
PcMachineStatus openxiso_machine_status(const PcMachine* machine);
const char* openxiso_machine_log(const PcMachine* machine);
const char* openxiso_machine_error(const PcMachine* machine);
void openxiso_machine_push_key(PcMachine* machine, uint16_t scan_and_ascii);
void openxiso_machine_stop(PcMachine* machine);
uint64_t openxiso_machine_instructions(const PcMachine* machine);

/* Framebuffer / VBE */
int openxiso_machine_get_framebuffer_info(const PcMachine* machine, int* width, int* height, int* bpp, int* pitch, int* mode);
int openxiso_machine_get_framebuffer(const PcMachine* machine, uint8_t* out, size_t capacity);
void openxiso_machine_set_vga_mode(PcMachine* machine, int mode, int width, int height, int bpp, int pitch);

/* Devices (devices.c) */
void devices_init(PcMachine* machine);
void devices_reset(PcMachine* machine);
uint8_t devices_port_in(PcMachine* machine, uint16_t port);
void devices_port_out(PcMachine* machine, uint16_t port, uint8_t value);
int devices_mmio_check(PcMachine* machine, uint64_t addr);
uint64_t devices_mmio_read(PcMachine* machine, uint64_t addr, int width);
void devices_mmio_write(PcMachine* machine, uint64_t addr, int width, uint64_t value);
int devices_irq_check(PcMachine* machine);
void devices_tick(PcMachine* machine, uint64_t instructions);
void devices_raise_irq(PcMachine* machine, int irq);
void devices_lower_irq(PcMachine* machine, int irq);
void devices_acpi_install(PcMachine* machine);
void devices_cmos_init(PcMachine* machine);
void devices_pci_init(PcMachine* machine);
void devices_ide_init(PcMachine* machine);
void devices_vga_init(PcMachine* machine);

#ifdef __cplusplus
}
#endif
#endif
