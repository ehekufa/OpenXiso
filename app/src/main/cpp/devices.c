#define _POSIX_C_SOURCE 200809L
#include "include/pc_machine.h"
#include "include/legacy_bios.h"
#include "include/boot_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ---------- PIC ---------- */
/* Fixed-priority cascaded 8259s. IRR, ISR and OCW3 are independent. */
static int first_bit(uint8_t bits) {
    for (int i = 0; i < 8; ++i) if (bits & (1u << i)) return i;
    return -1;
}
static int eligible(uint8_t irr, uint8_t mask, uint8_t isr) {
    int irq = first_bit(irr & (uint8_t)~mask);
    int in_service = first_bit(isr);
    return irq >= 0 && (in_service < 0 || irq < in_service) ? irq : -1;
}
static void pic_raise(PcMachine* m, int irq) {
    if (irq < 0 || irq >= 16) return;
    if (irq < 8) m->pic_irr |= (uint8_t)(1u << irq);
    else m->pic_irr_slave |= (uint8_t)(1u << (irq - 8));
}
static void pic_lower(PcMachine* m, int irq) {
    if (irq < 0 || irq >= 16) return;
    if (irq < 8) m->pic_irr &= (uint8_t)~(1u << irq);
    else m->pic_irr_slave &= (uint8_t)~(1u << (irq - 8));
}
void devices_raise_irq(PcMachine* m, int irq) { pic_raise(m, irq); }
void devices_lower_irq(PcMachine* m, int irq) { pic_lower(m, irq); }

static uint8_t pic_read(PcMachine* m, int slave, int command) {
    if (!command) return slave ? m->pic_imr_slave : m->pic_imr_master;
    uint8_t select = slave ? m->pic_ocw3_slave : m->pic_ocw3_master;
    if (select & 1) return slave ? m->pic_isr_slave : m->pic_isr;
    uint8_t irr = slave ? m->pic_irr_slave : m->pic_irr;
    if (!slave && (m->pic_irr_slave & (uint8_t)~m->pic_imr_slave)) irr |= 4;
    return irr;
}
static void pic_write(PcMachine* m, int slave, int command, uint8_t value) {
    int* step = slave ? &m->pic_icw_step_slave : &m->pic_icw_step_master;
    int* icw4 = slave ? &m->pic_icw4_needed_slave : &m->pic_icw4_needed_master;
    uint8_t* irr = slave ? &m->pic_irr_slave : &m->pic_irr;
    uint8_t* isr = slave ? &m->pic_isr_slave : &m->pic_isr;
    if (command) {
        if (value & 0x10) {
            *step = 1; *icw4 = !!(value & 1); *irr = 0; *isr = 0;
            if (!slave) m->pic_single = !!(value & 2);
        } else if (value & 8) {
            if (value & 2) {
                if (slave) m->pic_ocw3_slave = value;
                else m->pic_ocw3_master = value;
            }
        } else if (value & 0x20) {
            int irq = (value & 0x40) ? (value & 7) : first_bit(*isr);
            if (irq >= 0) *isr &= (uint8_t)~(1u << irq);
        }
        return;
    }
    if (*step == 1) {
        if (slave) m->pic_base_slave = value & 0xf8;
        else m->pic_base_master = value & 0xf8;
        *step = m->pic_single ? (*icw4 ? 3 : 0) : 2;
    } else if (*step == 2) *step = *icw4 ? 3 : 0;
    else if (*step == 3) *step = 0;
    else if (slave) m->pic_imr_slave = value;
    else m->pic_imr_master = value;
}
int devices_irq_check(PcMachine* m) {
    if (m->lapic_timer_pending && m->lapic_timer_vector) {
        m->lapic_timer_pending = 0;
        return m->lapic_timer_vector;
    }
    int slave = eligible(m->pic_irr_slave, m->pic_imr_slave, m->pic_isr_slave);
    uint8_t pending = m->pic_irr | (slave >= 0 ? 4u : 0u);
    int master = eligible(pending, m->pic_imr_master, m->pic_isr);
    if (master < 0) return -1;
    m->pic_isr |= (uint8_t)(1u << master);
    if (master == 2 && slave >= 0) {
        m->pic_irr_slave &= (uint8_t)~(1u << slave);
        m->pic_isr_slave |= (uint8_t)(1u << slave);
        return m->pic_base_slave + slave;
    }
    m->pic_irr &= (uint8_t)~(1u << master);
    return m->pic_base_master + master;
}

/* ---------- PIT ---------- */
static void pit_tick_channel(PcMachine* m, int ch) {
    if (m->pit_mode[ch] == 0 || m->pit_mode[ch] == 4) return;
    if (!m->pit_gate[ch]) return;
    if (m->pit_count[ch] == 0) {
        m->pit_count[ch] = m->pit_reload[ch] ? m->pit_reload[ch] : 65536;
        if (ch == 0) pic_raise(m, 0);
        if (ch == 2) { /* speaker: ignore */ }
    } else {
        m->pit_count[ch]--;
        if (m->pit_count[ch] == 0) {
            m->pit_count[ch] = m->pit_reload[ch] ? m->pit_reload[ch] : 65536;
            if (ch == 0) pic_raise(m, 0);
        }
    }
}
static void pit_write(PcMachine* m, int ch, uint8_t value) {
    int access = (m->pit_access[ch] >> 4) & 3;
    if (access == 1) { /* low only */
        m->pit_reload[ch] = (m->pit_reload[ch] & 0xFF00) | value;
        m->pit_count[ch] = m->pit_reload[ch];
    } else if (access == 2) { /* high only */
        m->pit_reload[ch] = (m->pit_reload[ch] & 0x00FF) | (value << 8);
        m->pit_count[ch] = m->pit_reload[ch];
    } else if (access == 3) { /* low then high */
        if (!m->pit_latch_high[ch]) {
            m->pit_reload[ch] = (m->pit_reload[ch] & 0xFF00) | value;
            m->pit_latch_high[ch] = 1;
        } else {
            m->pit_reload[ch] = (m->pit_reload[ch] & 0x00FF) | (value << 8);
            m->pit_count[ch] = m->pit_reload[ch];
            m->pit_latch_high[ch] = 0;
        }
    } else { /* latch command */
        /* value is latch command, ignore */
    }
}
static uint8_t pit_read(PcMachine* m, int ch) {
    int access = (m->pit_access[ch] >> 4) & 3;
    if (access == 1) return (uint8_t)(m->pit_count[ch] & 0xFF);
    if (access == 2) return (uint8_t)(m->pit_count[ch] >> 8);
    if (access == 3) {
        if (!m->pit_read_pending[ch]) {
            m->pit_read_latch[ch] = (uint8_t)(m->pit_count[ch] & 0xFF);
            m->pit_read_pending[ch] = 1;
            return m->pit_read_latch[ch];
        } else {
            m->pit_read_pending[ch] = 0;
            return (uint8_t)(m->pit_count[ch] >> 8);
        }
    }
    return 0;
}
static void pit_command(PcMachine* m, uint8_t value) {
    int ch = (value >> 6) & 3;
    int access = (value >> 4) & 3;
    int mode = (value >> 1) & 7;
    if (ch == 3) return;
    if (access == 0) { /* latch */
        m->pit_latch[ch] = (uint8_t)(m->pit_count[ch] & 0xFF);
        m->pit_latch_pending[ch] = 1;
        return;
    }
    m->pit_access[ch] = value;
    m->pit_mode[ch] = mode;
    m->pit_latch_high[ch] = 0;
    m->pit_read_pending[ch] = 0;
    if (mode == 0 || mode == 4) {
        /* software trigger: count loaded on write */
    }
}

/* ---------- Keyboard ---------- */
static uint8_t kbd_status(PcMachine* m) {
    uint8_t s = 0;
    if (m->kbd_output_full) s |= 0x01;
    if (m->kbd_input_full) s |= 0x02;
    if (m->kbd_expect_mouse) s |= 0x20;
    return s;
}

/* ---------- CMOS ---------- */
static uint8_t cmos_read(PcMachine* m, uint8_t idx) {
    if (idx == 0x00) { /* seconds */ time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)tm->tm_sec; }
    if (idx == 0x02) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)tm->tm_min; }
    if (idx == 0x04) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)tm->tm_hour; }
    if (idx == 0x06) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)tm->tm_wday; }
    if (idx == 0x07) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)tm->tm_mday; }
    if (idx == 0x09) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)(tm->tm_mon+1); }
    if (idx == 0x32) { time_t t=time(NULL); struct tm* tm=gmtime(&t); return (uint8_t)((tm->tm_year+1900)%100); }
    if (idx == 0x0B) return 0x02; /* status B: 24h, binary */
    if (idx == 0x0A) return 0x80; /* status A: UIP=0 */
    return m->cmos_memory[idx & 0x7F];
}
static void cmos_write(PcMachine* m, uint8_t idx, uint8_t value) {
    m->cmos_memory[idx & 0x7F] = value;
}

/* ---------- DMA ---------- */
static void dma_write(PcMachine* m, uint16_t port, uint8_t value) {
    if (port == 0x0B) { m->dma_mode[0] = value; return; }
    if (port == 0x0C) { m->dma_flipflop = 0; return; }
    if (port == 0x0D) { m->dma_flipflop = 0; return; }
    if (port == 0x0A) { /* mask */
        /* value bit2 = mask, bits0-1 channel */
        int ch = value & 3;
        if (value & 4) m->dma_mask |= (1 << ch); else m->dma_mask &= ~(1 << ch);
        return;
    }
    if (port == 0x08) { m->dma_mask = value; return; }
    if (port == 0x09) { /* single mask */ return; }
    if (port == 0x0F) { /* demand mask */ return; }
    if (port <= 0x07) {
        int ch = port & 3;
        int is_count = (port & 4) ? 1 : 0;
        if (!m->dma_flipflop) {
            if (is_count) m->dma_count[ch] = (m->dma_count[ch] & 0xFF00) | value;
            else m->dma_addr[ch] = (m->dma_addr[ch] & 0xFF00) | value;
            m->dma_flipflop = 1;
        } else {
            if (is_count) m->dma_count[ch] = (m->dma_count[ch] & 0x00FF) | (value << 8);
            else m->dma_addr[ch] = (m->dma_addr[ch] & 0x00FF) | (value << 8);
            m->dma_flipflop = 0;
        }
        return;
    }
    if (port == 0x80 || port == 0x81 || port == 0x82 || port == 0x83) {
        int ch = port & 3;
        m->dma_page[ch] = value;
        return;
    }
    if (port == 0x8C || port == 0x8D || port == 0x8E || port == 0x8F) {
        int ch = port & 3;
        if (!m->dma_flipflop) { m->dma_addr_high[ch] = value; m->dma_flipflop=1; }
        else { m->dma_count_high[ch] = value; m->dma_flipflop=0; }
        return;
    }
}
static uint8_t dma_read(PcMachine* m, uint16_t port) {
    if (port == 0x08) return m->dma_mask;
    if (port == 0x0D) return 0;
    if (port <= 0x07) {
        int ch = port & 3;
        int is_count = (port & 4) ? 1 : 0;
        uint8_t v;
        if (!m->dma_flipflop) v = is_count ? (m->dma_count[ch] & 0xFF) : (m->dma_addr[ch] & 0xFF);
        else v = is_count ? (m->dma_count[ch] >> 8) : (m->dma_addr[ch] >> 8);
        m->dma_flipflop = !m->dma_flipflop;
        return v;
    }
    if (port == 0x80 || port == 0x81 || port == 0x82 || port == 0x83) return m->dma_page[port & 3];
    return 0;
}

/* ---------- PCI ---------- */
static void pci_write_config(PcMachine* m, uint32_t addr, uint8_t value) {
    unsigned bus = (addr >> 16) & 0xff, dev = (addr >> 11) & 31;
    unsigned func = (addr >> 8) & 7, off = addr & 0xff;
    if (!(addr & 0x80000000u) || bus || !m->pci_present[dev][func]) return;
    /* Identification and class registers are read-only. */
    if (off < 4 || (off >= 8 && off < 12)) return;
    m->pci_config[dev][func][off] = value;
}
static uint8_t pci_read_config(PcMachine* m, uint32_t addr) {
    unsigned bus = (addr >> 16) & 0xff, dev = (addr >> 11) & 31;
    unsigned func = (addr >> 8) & 7, off = addr & 0xff;
    if (!(addr & 0x80000000u) || bus || !m->pci_present[dev][func]) return 0xff;
    return m->pci_config[dev][func][off];
}
static void pci_set_word(PcMachine* m, int dev, int func, int off, uint16_t v) {
    m->pci_config[dev][func][off] = v & 0xFF;
    m->pci_config[dev][func][off+1] = v >> 8;
}
static void pci_set_dword(PcMachine* m, int dev, int func, int off, uint32_t v) {
    for(int i=0;i<4;i++) m->pci_config[dev][func][off+i] = (v>>(8*i))&0xFF;
}
static void pci_set_byte(PcMachine* m, int dev, int func, int off, uint8_t v) { m->pci_config[dev][func][off]=v; }

void devices_pci_init(PcMachine* m) {
    memset(m->pci_config, 0, sizeof(m->pci_config));
    memset(m->pci_present, 0, sizeof(m->pci_present));
    /* Device 0: Host bridge 8086:1237 */
    m->pci_present[0][0]=1;
    pci_set_word(m,0,0,0,0x8086); pci_set_word(m,0,0,2,0x1237);
    pci_set_byte(m,0,0,0x0A,0x00); pci_set_byte(m,0,0,0x0B,0x06); pci_set_byte(m,0,0,0x08,0x00);
    pci_set_byte(m,0,0,0x3E,0x0B); /* IRQ */
    /* Device 1: VGA 8086:1234 */
    m->pci_present[1][0]=1;
    pci_set_word(m,1,0,0,0x8086); pci_set_word(m,1,0,2,0x1234);
    pci_set_byte(m,1,0,0x0A,0x00); pci_set_byte(m,1,0,0x0B,0x03); pci_set_byte(m,1,0,0x08,0x00);
    pci_set_dword(m,1,0,0x10, OPENXISO_LFB_BASE | 0x08); /* BAR0 mem 32-bit */
    pci_set_dword(m,1,0,0x14, 0xE4000000 | 0x08);
    pci_set_byte(m,1,0,0x3E,0x0B);
    /* Device 2: IDE 8086:7010 */
    m->pci_present[2][0]=1;
    pci_set_word(m,2,0,0,0x8086); pci_set_word(m,2,0,2,0x7010);
    pci_set_byte(m,2,0,0x0A,0x80); pci_set_byte(m,2,0,0x0B,0x01); pci_set_byte(m,2,0,0x08,0x00);
    pci_set_dword(m,2,0,0x10, 0xC000 | 0x01); /* BAR0 IO */
    pci_set_dword(m,2,0,0x14, 0xC008 | 0x01);
    pci_set_dword(m,2,0,0x1C, 0xC010 | 0x01);
    pci_set_dword(m,2,0,0x20, 0xC018 | 0x01);
    pci_set_dword(m,2,0,0x24, 0xC020 | 0x01);
    pci_set_byte(m,2,0,0x3E,0x0B);
    /* Device 3: Bridge ISA 8086:7000 */
    m->pci_present[3][0]=1;
    pci_set_word(m,3,0,0,0x8086); pci_set_word(m,3,0,2,0x7000);
    pci_set_byte(m,3,0,0x0A,0x80); pci_set_byte(m,3,0,0x0B,0x06); pci_set_byte(m,3,0,0x08,0x00);
}

/* ---------- IDE ---------- */
static uint64_t ide_disk_sectors(PcMachine* m) {
    if (m->plan.kind == OPENXISO_IMAGE_ISO && m->ide_atapi[0]) {
        return m->plan.image_bytes / 2048;
    }
    uint64_t bytes = m->plan.image_bytes;
    if (bytes==0) bytes = (uint64_t)m->image_size;
    return bytes / 512;
}
static int ide_read_disk(PcMachine* m, uint64_t lba, uint32_t count, uint8_t* buf, int atapi) {
    uint32_t sector_size = atapi ? 2048 : 512;
    uint64_t offset;
    if (m->plan.kind == OPENXISO_IMAGE_ISO && !atapi) {
        /* HDD emulation of El Torito image */
        offset = m->plan.image_offset + lba * 512;
    } else if (m->plan.kind == OPENXISO_IMAGE_ISO && atapi) {
        offset = lba * 2048;
    } else {
        offset = lba * 512;
    }
    size_t len = (size_t)count * sector_size;
    if (len > 65536) return -1;
    /* Use pread via boot_image helper? Direct pread */
    ssize_t done = 0;
    while ((size_t)done < len) {
        ssize_t r = pread(m->fd, buf+done, len-done, (off_t)offset+done);
        if (r < 0) return -1;
        if (r==0) break;
        done += r;
    }
    if ((size_t)done < len) return -1;
    return (int)len;
}
static void ide_set_status(PcMachine* m, int ch, int bsy, int drdy, int drq, int err) {
    uint8_t s = 0;
    if (bsy) s|=0x80;
    if (drdy) s|=0x40;
    if (drq) s|=0x08;
    if (err) s|=0x01;
    /* DSC=1 when ready */
    if (drdy) s|=0x10;
    m->ide_status[ch]=s;
}
static void ide_execute(PcMachine* m, int ch) {
    uint8_t cmd = m->ide_command[ch];
    uint32_t count = m->ide_sector_count[ch];
    uint64_t lba = ((uint64_t)(m->ide_drive_head[ch] & 0x0F) << 24) | ((uint64_t)m->ide_lba[ch][2] << 16) | ((uint64_t)m->ide_lba[ch][1] << 8) | m->ide_lba[ch][0];
    /* LBA28: drive/head bits 24-27 are LBA 24-27 */
    if (!(m->ide_drive_head[ch] & 0x40)) {
        /* CHS mode - convert */
        int head = m->ide_drive_head[ch] & 0x0F;
        int sector = m->ide_lba[ch][0];
        int cyl = m->ide_lba[ch][1] | (m->ide_lba[ch][2] << 8);
        int spt = m->plan.sectors_per_track ? m->plan.sectors_per_track : 63;
        int heads = m->plan.heads ? m->plan.heads : 16;
        lba = ((uint64_t)cyl * heads + head) * spt + (sector>0?sector-1:0);
    }
    if (cmd == 0xEC) { /* IDENTIFY */
        memset(m->ide_identify[ch],0,sizeof(m->ide_identify[ch]));
        m->ide_identify[ch][0]=0x0040;
        m->ide_identify[ch][10]=0x2020; m->ide_identify[ch][11]=0x2020; m->ide_identify[ch][12]=0x2020;
        /* Model: OPENXISO DISK */
        const char model[41]="OPENXISO DISK                           ";
        for(int i=0;i<20;i++){ char a=model[i*2], b=model[i*2+1]; m->ide_identify[ch][23+i]= (a<<8)|b; }
        uint64_t secs = ide_disk_sectors(m);
        m->ide_identify[ch][60]=(uint16_t)(secs & 0xFFFF);
        m->ide_identify[ch][61]=(uint16_t)(secs>>16);
        m->ide_identify[ch][83]=(1<<10)|(1<<9);
        m->ide_identify[ch][88]=0x0007;
        m->ide_identify[ch][100]=(uint16_t)(secs & 0xFFFF);
        m->ide_identify[ch][101]=(uint16_t)(secs>>16);
        m->ide_identify[ch][102]=(uint16_t)(secs>>32);
        m->ide_identify[ch][103]=(uint16_t)(secs>>48);
        m->ide_buffer_pos=0; m->ide_buffer_len=512;
        for(int i=0;i<256;i++){ m->ide_buffer[i*2]=m->ide_identify[ch][i]&0xFF; m->ide_buffer[i*2+1]=m->ide_identify[ch][i]>>8; }
        ide_set_status(m,ch,0,1,1,0);
        m->ide_drq[ch]=1;
        if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
        return;
    }
    if (cmd == 0xA1) { /* ATAPI IDENTIFY PACKET */
        memset(m->ide_identify[ch],0,sizeof(m->ide_identify[ch]));
        m->ide_identify[ch][0]=0x0040;
        const char model[41]="OPENXISO CD-ROM                         ";
        for(int i=0;i<20;i++){ char a=model[i*2], b=model[i*2+1]; m->ide_identify[ch][23+i]= (a<<8)|b; }
        m->ide_buffer_pos=0; m->ide_buffer_len=512;
        for(int i=0;i<256;i++){ m->ide_buffer[i*2]=m->ide_identify[ch][i]&0xFF; m->ide_buffer[i*2+1]=m->ide_identify[ch][i]>>8; }
        ide_set_status(m,ch,0,1,1,0); m->ide_drq[ch]=1;
        if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
        return;
    }
    if (cmd == 0x20 || cmd == 0x21 || cmd == 0x25 || cmd == 0xA8) { /* READ SECTORS / READ DMA / ATAPI READ CAPACITY */
        int atapi = m->ide_atapi[ch];
        if (cmd==0xA8) { /* READ CAPACITY */
            uint64_t secs = ide_disk_sectors(m);
            m->ide_buffer[0]=(secs>>24)&0xFF; m->ide_buffer[1]=(secs>>16)&0xFF; m->ide_buffer[2]=(secs>>8)&0xFF; m->ide_buffer[3]=secs&0xFF;
            m->ide_buffer[4]=0; m->ide_buffer[5]=0; m->ide_buffer[6]=0x08; m->ide_buffer[7]=0x00; /* 2048 */
            m->ide_buffer_pos=0; m->ide_buffer_len=8;
            ide_set_status(m,ch,0,1,1,0); m->ide_drq[ch]=1;
            if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
            return;
        }
        if (count==0) count=256;
        int len = ide_read_disk(m,lba,count,m->ide_buffer,atapi);
        if (len<0){ ide_set_status(m,ch,0,1,0,1); m->ide_error[ch]=0x04; return; }
        m->ide_buffer_pos=0; m->ide_buffer_len=len;
        ide_set_status(m,ch,0,1,1,0); m->ide_drq[ch]=1;
        if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
        return;
    }
    if (cmd == 0x28 || cmd == 0xA8) { /* READ(10) ATAPI */
        uint32_t lba32 = ((uint32_t)m->ide_lba[ch][3]<<24)|((uint32_t)m->ide_lba[ch][2]<<16)|((uint32_t)m->ide_lba[ch][1]<<8)|m->ide_lba[ch][0];
        /* Actually ATAPI READ(10) packet is via DMA/PIO packet, simplified: use LBA from packet? Use lba32 */
        int len = ide_read_disk(m,lba32,count?count:1,m->ide_buffer,1);
        if(len<0){ide_set_status(m,ch,0,1,0,1); return;}
        m->ide_buffer_pos=0; m->ide_buffer_len=len;
        ide_set_status(m,ch,0,1,1,0); m->ide_drq[ch]=1;
        if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
        return;
    }
    if (cmd == 0x30 || cmd == 0x35) { /* WRITE */
        ide_set_status(m,ch,0,1,0,1); m->ide_error[ch]=0x04; m->ide_drq[ch]=0;
        return;
    }
    if (cmd == 0x91) { ide_set_status(m,ch,0,1,0,0); return; }
    if (cmd == 0xEF) { ide_set_status(m,ch,0,1,0,0); return; }
    if (cmd == 0x12) { /* INQUIRY ATAPI */
        memset(m->ide_buffer,0,36);
        m->ide_buffer[0]=0x05; m->ide_buffer[2]=0x05;
        memcpy(m->ide_buffer+8,"OPENXISO",8);
        memcpy(m->ide_buffer+16,"CD-ROM            ",16);
        m->ide_buffer_pos=0; m->ide_buffer_len=36;
        ide_set_status(m,ch,0,1,1,0); m->ide_drq[ch]=1;
        if (!(m->ide_device_control[ch] & 0x02)) pic_raise(m,14+ch);
        return;
    }
    if (cmd == 0x00) { ide_set_status(m,ch,0,1,0,0); return; }
    /* Unknown: error */
    ide_set_status(m,ch,0,1,0,1);
    m->ide_error[ch]=0x04;
}
static uint8_t ide_port_in(PcMachine* m, uint16_t port) {
    int ch = (port == 0x376 || (port >= 0x170 && port <= 0x177)) ? 1 : 0;
    int reg = port & 7;
    if (port==0x3F6 || port==0x376) return m->ide_status[ch];
    if (reg==0) { /* data */
        if (m->ide_buffer_pos < m->ide_buffer_len) {
            uint8_t v = m->ide_buffer[m->ide_buffer_pos++];
            if (m->ide_buffer_pos>=m->ide_buffer_len) { m->ide_drq[ch]=0; ide_set_status(m,ch,0,1,0,0); }
            return v;
        }
        return 0;
    }
    if (reg==7) return m->ide_status[ch];
    if (reg==6) return m->ide_drive_head[ch];
    if (reg==1) return m->ide_error[ch];
    if (reg==2) return m->ide_sector_count[ch];
    if (reg==3) return m->ide_lba[ch][0];
    if (reg==4) return m->ide_lba[ch][1];
    if (reg==5) return m->ide_lba[ch][2];
    return 0;
}
static void ide_port_out(PcMachine* m, uint16_t port, uint8_t v) {
    int ch = (port == 0x376 || (port >= 0x170 && port <= 0x177)) ? 1 : 0;
    int reg = port & 7;
    if (port==0x3F6 || port==0x376) { m->ide_device_control[ch]=v; return; }
    if (reg==0) { /* data write - for WRITE command, consume */
        if (m->ide_buffer_pos < 65536) m->ide_buffer[m->ide_buffer_pos++]=v;
        return;
    }
    if (reg==1) { m->ide_features[ch]=v; return; }
    if (reg==2) { m->ide_sector_count[ch]=v; return; }
    if (reg==3) { m->ide_lba[ch][0]=v; return; }
    if (reg==4) { m->ide_lba[ch][1]=v; return; }
    if (reg==5) { m->ide_lba[ch][2]=v; return; }
    if (reg==6) { m->ide_drive_head[ch]=v; m->ide_lba[ch][3]=v & 0x0F; return; }
    if (reg==7) { m->ide_command[ch]=v; ide_execute(m,ch); return; }
}

void devices_ide_init(PcMachine* m) {
    memset(m->ide_regs,0,sizeof(m->ide_regs));
    memset(m->ide_status,0,sizeof(m->ide_status));
    memset(m->ide_error,0,sizeof(m->ide_error));
    memset(m->ide_alt_status,0,sizeof(m->ide_alt_status));
    memset(m->ide_device_control,0,sizeof(m->ide_device_control));
    memset(m->ide_drq,0,sizeof(m->ide_drq));
    memset(m->ide_atapi,0,sizeof(m->ide_atapi));
    memset(m->ide_identify,0,sizeof(m->ide_identify));
    memset(m->ide_buffer,0,sizeof(m->ide_buffer));
    m->ide_buffer_pos=0; m->ide_buffer_len=0;
    /* Primary master present */
    ide_set_status(m,0,0,1,0,0);
    m->ide_drive_head[0]=0xE0;
    if (m->plan.kind == OPENXISO_IMAGE_ISO) m->ide_atapi[0]=1;
}

/* ---------- VGA ---------- */
void devices_vga_init(PcMachine* m) {
    m->vga_mode=3; m->vga_width=80; m->vga_height=25; m->vga_bpp=4; m->vga_pitch=80;
    m->vga_lfb_phys=OPENXISO_LFB_BASE;
    m->vga_bank=0; m->vga_bank_size=65536;
    memset(m->vga_palette,0,sizeof(m->vga_palette));
    for(int i=0;i<16;i++){ m->vga_palette[i][0]=i*4; m->vga_palette[i][1]=i*4; m->vga_palette[i][2]=i*4; }
    memset(m->vga_seq,0,sizeof(m->vga_seq));
    memset(m->vga_crtc,0,sizeof(m->vga_crtc));
    memset(m->vga_gc,0,sizeof(m->vga_gc));
    memset(m->vga_attr,0,sizeof(m->vga_attr));
    m->vga_misc=0x63;
    m->framebuffer_size=OPENXISO_LFB_SIZE;
    if (!m->framebuffer) m->framebuffer=(uint8_t*)calloc(m->framebuffer_size,1);
    else memset(m->framebuffer, 0, m->framebuffer_size);
    m->fb_width=80; m->fb_height=25; m->fb_bpp=4; m->fb_pitch=80; m->fb_mode=0;
    m->vbe_lfb_enabled=0;
}
void openxiso_machine_set_vga_mode(PcMachine* m, int mode, int width, int height, int bpp, int pitch) {
    m->vga_mode=mode; m->vga_width=width; m->vga_height=height; m->vga_bpp=bpp; m->vga_pitch=pitch;
    m->fb_width=width; m->fb_height=height; m->fb_bpp=bpp; m->fb_pitch=pitch;
    if (mode==0x13) m->fb_mode=1;
    else if (mode>=0x100) m->fb_mode=2;
    else m->fb_mode=0;
    if (m->framebuffer) {
        size_t need = (size_t)pitch * height;
        if (need > m->framebuffer_size) need = m->framebuffer_size;
        memset(m->framebuffer,0,need);
    }
}
int openxiso_machine_get_framebuffer_info(const PcMachine* m, int* w,int* h,int* bpp,int* pitch,int* mode){
    if (w) *w = m->fb_width;
    if (h) *h = m->fb_height;
    if (bpp) *bpp = m->fb_bpp;
    if (pitch) *pitch = m->fb_pitch;
    if (mode) *mode = m->fb_mode;
    return m->fb_mode!=0;
}
int openxiso_machine_get_framebuffer(const PcMachine* m, uint8_t* out, size_t cap){
    if(!m->framebuffer||!out) return -1;
    size_t n = m->framebuffer_size < cap ? m->framebuffer_size : cap;
    memcpy(out,m->framebuffer,n);
    return (int)n;
}
static uint8_t vga_port_in(PcMachine* m, uint16_t port){
    if(port==0x3C2) return m->vga_misc;
    if(port==0x3C4) return m->vga_seq[m->vga_dac_index & 31];
    if(port==0x3D4 || port==0x3B4) return m->vga_crtc[m->vga_dac_index & 31];
    if(port==0x3CE) return m->vga_gc[m->vga_dac_index & 31];
    if(port==0x3C8) return 0;
    if(port==0x3C9) { uint8_t v=m->vga_palette[m->vga_dac_read_index & 255][m->vga_dac_state]; m->vga_dac_state=(m->vga_dac_state+1)%3; if(m->vga_dac_state==0) m->vga_dac_read_index++; return v; }
    if(port==0x3DA || port==0x3BA) return 0x08; /* retrace */
    return 0xFF;
}
static void vga_port_out(PcMachine* m, uint16_t port, uint8_t v){
    if(port==0x3C0){ m->vga_attr[m->vga_dac_index & 31]=v; m->vga_dac_index++; return; }
    if(port==0x3C2){ m->vga_misc=v; return; }
    if(port==0x3C4){ m->vga_seq[m->vga_dac_index & 31]=v; return; }
    if(port==0x3C5){ m->vga_seq[m->vga_dac_index & 31]=v; return; }
    if(port==0x3D4 || port==0x3B4){ m->vga_dac_index=v; return; }
    if(port==0x3D5 || port==0x3B5){ m->vga_crtc[m->vga_dac_index & 31]=v; return; }
    if(port==0x3CE){ m->vga_dac_index=v; return; }
    if(port==0x3CF){ m->vga_gc[m->vga_dac_index & 31]=v; return; }
    if(port==0x3C8){ m->vga_dac_index=v; m->vga_dac_read_index=v; m->vga_dac_state=0; return; }
    if(port==0x3C9){ m->vga_palette[m->vga_dac_index & 255][m->vga_dac_state]=v; m->vga_dac_state++; if(m->vga_dac_state>=3){m->vga_dac_state=0; m->vga_dac_index++;} return; }
    if(port==0x3C6){ return; }
    if(port==0x3C7){ return; }
}

/* ---------- APIC / IOAPIC / HPET MMIO ---------- */
static int is_mmio_addr(uint64_t addr){
    if(addr>=OPENXISO_LAPIC_BASE && addr<OPENXISO_LAPIC_BASE+0x1000) return 1;
    if(addr>=OPENXISO_IOAPIC_BASE && addr<OPENXISO_IOAPIC_BASE+0x1000) return 1;
    if(addr>=OPENXISO_HPET_BASE && addr<OPENXISO_HPET_BASE+0x1000) return 1;
    if(addr>=OPENXISO_LFB_BASE && addr<OPENXISO_LFB_BASE+OPENXISO_LFB_SIZE) return 1;
    return 0;
}
int devices_mmio_check(PcMachine* m, uint64_t addr){ (void)m; return is_mmio_addr(addr); }
uint64_t devices_mmio_read(PcMachine* m, uint64_t addr, int width){
    if(addr>=OPENXISO_LFB_BASE && addr<OPENXISO_LFB_BASE+OPENXISO_LFB_SIZE){
        uint64_t off=addr-OPENXISO_LFB_BASE;
        if(!m->framebuffer || off>=m->framebuffer_size || (size_t)(width/8) > m->framebuffer_size-off) return 0;
        uint64_t v=0; for(int i=0;i<width/8;i++) v|=((uint64_t)m->framebuffer[off+i])<<(8*i);
        return v;
    }
    if(addr>=OPENXISO_LAPIC_BASE && addr<OPENXISO_LAPIC_BASE+0x1000){
        int reg=(addr-OPENXISO_LAPIC_BASE)/16;
        if(reg==0x0B) return 0x01000014; /* version */
        if(reg==0x08) return 0; /* TPR */
        if(reg==0x0A) return 0; /* EOI */
        if(reg==0xB0) return m->lapic_regs[reg];
        if(reg==0xB8) return m->lapic_timer_count;
        if(reg==0xB9) return m->lapic_timer_init;
        if(reg==0xBA) return m->lapic_timer_mode | (m->lapic_timer_vector);
        if(reg==0xD0) return 0x01000000; /* ID */
        return m->lapic_regs[reg];
    }
    if(addr>=OPENXISO_IOAPIC_BASE && addr<OPENXISO_IOAPIC_BASE+0x1000){
        /* IOAPIC reg select at 0, window at 0x10 */
        static uint32_t ioapic_sel=0;
        if(addr==OPENXISO_IOAPIC_BASE) return ioapic_sel;
        if(addr==OPENXISO_IOAPIC_BASE+0x10){
            if(ioapic_sel>=0x10 && ioapic_sel<0x10+24*2){
                int idx=(ioapic_sel-0x10)/2;
                int hi=(ioapic_sel-0x10)%2;
                return m->ioapic_redtbl[idx][hi];
            }
            if(ioapic_sel==0x01) return 0x00110001; /* version: 1 redir, version 1 */
            if(ioapic_sel==0x02) return 24; /* max redir */
            return 0;
        }
        return 0;
    }
    if(addr>=OPENXISO_HPET_BASE && addr<OPENXISO_HPET_BASE+0x1000){
        int reg=(addr-OPENXISO_HPET_BASE)/8;
        if(reg==0) return 0x0000000A00000001ULL; /* capabilities: 64-bit, 3 timers */
        if(reg==1) return 0; /* config */
        if(reg==2) return 0; /* intr status */
        if(reg==0xF0/8) return m->hpet_counter_low;
        return reg < 256 ? m->hpet_regs[reg] : 0;
    }
    return 0;
}
void devices_mmio_write(PcMachine* m, uint64_t addr, int width, uint64_t value){
    if(addr>=OPENXISO_LFB_BASE && addr<OPENXISO_LFB_BASE+OPENXISO_LFB_SIZE){
        uint64_t off=addr-OPENXISO_LFB_BASE;
        if(!m->framebuffer || off>=m->framebuffer_size || (size_t)(width/8) > m->framebuffer_size-off) return;
        for(int i=0;i<width/8;i++) m->framebuffer[off+i]=(uint8_t)(value>>(8*i));
        return;
    }
    if(addr>=OPENXISO_LAPIC_BASE && addr<OPENXISO_LAPIC_BASE+0x1000){
        int reg=(addr-OPENXISO_LAPIC_BASE)/16;
        m->lapic_regs[reg]=(uint32_t)value;
        if(reg==0x0B){ /* EOI */ }
        if(reg==0xB8){ m->lapic_timer_count=(uint32_t)value; }
        if(reg==0xB9){ m->lapic_timer_init=(uint32_t)value; m->lapic_timer_count=(uint32_t)value; }
        if(reg==0xBA){ m->lapic_timer_mode=(uint8_t)value; m->lapic_timer_vector=(uint8_t)value; }
        if(reg==0x80){ /* TPR */ }
        return;
    }
    if(addr>=OPENXISO_IOAPIC_BASE && addr<OPENXISO_IOAPIC_BASE+0x1000){
        static uint32_t ioapic_sel=0;
        if(addr==OPENXISO_IOAPIC_BASE){ ioapic_sel=(uint32_t)value; return; }
        if(addr==OPENXISO_IOAPIC_BASE+0x10){
            if(ioapic_sel>=0x10 && ioapic_sel<0x10+24*2){
                int idx=(ioapic_sel-0x10)/2;
                int hi=(ioapic_sel-0x10)%2;
                m->ioapic_redtbl[idx][hi]=(uint32_t)value;
                /* If redirection entry unmasked and source is PIC IRQ, route? Simplify: if vector set and not masked, raise PIC IRQ corresponding */
                /* IOAPIC entries for ISA IRQs 0-15 map to PIC */
                if(hi==0){
                    uint32_t low=m->ioapic_redtbl[idx][0];
                    if(!(low & (1ULL<<0)) && (low & 0xFF)){
                        /* vector present, unmasked - raise IRQ if idx<16 */
                        if(idx<16) pic_raise(m,idx);
                    }
                }
            }
            return;
        }
        return;
    }
    if(addr>=OPENXISO_HPET_BASE && addr<OPENXISO_HPET_BASE+0x1000){
        int reg=(addr-OPENXISO_HPET_BASE)/8;
        if (reg < 256) m->hpet_regs[reg]=(uint32_t)value;
        if(reg==1) m->hpet_enabled = (value & 1) ? 1 : 0;
        return;
    }
}

/* ---------- ACPI ---------- */
/* ACPI is not advertised until valid tables and devices are implemented. */
void devices_acpi_install(PcMachine* m) {
    m->acpi_rsdp_addr = 0;
    m->acpi_installed = 0;
}

void devices_cmos_init(PcMachine* m){
    memset(m->cmos_memory,0,sizeof(m->cmos_memory));
    m->cmos_memory[0x10]=0x2D; /* equipment: 1 floppy, 80x25, 1 printer? */
    m->cmos_memory[0x12]=0x80; m->cmos_memory[0x13]=0x02; /* base mem 640K */
    m->cmos_memory[0x14]=0x01; /* 1 floppy */
    m->cmos_memory[0x15]=0x00;
    m->cmos_memory[0x17]=0x00;
    m->cmos_memory[0x30]=0x00; m->cmos_memory[0x31]=0x3C; /* extended mem low: 60*? 0x3C00 = 15360K? Actually 0x30/31 = extended mem KB low */
    m->cmos_memory[0x30]=0x00; m->cmos_memory[0x31]=0x3C;
    m->cmos_memory[0x32]=0x00; m->cmos_memory[0x33]=0x00;
    m->cmos_memory[0x34]=0x1E; /* extended mem high */
    m->cmos_memory[0x35]=0x00;
}

/* ---------- Init / Reset ---------- */
void devices_init(PcMachine* m){
    m->pic_base_master=0x08; m->pic_base_slave=0x70;
    m->pic_imr_master=0xFF; m->pic_imr_slave=0xFF;
    m->pic_irr=0; m->pic_isr=0; m->pic_irr_slave=0; m->pic_isr_slave=0;
    m->pic_icw_step_master=0; m->pic_icw_step_slave=0;
    m->pic_ocw3_master=0; m->pic_ocw3_slave=0;
    memset(m->pit_count,0,sizeof(m->pit_count));
    memset(m->pit_reload,0,sizeof(m->pit_reload));
    memset(m->pit_mode,0,sizeof(m->pit_mode));
    memset(m->pit_access,0,sizeof(m->pit_access));
    memset(m->pit_gate,0,sizeof(m->pit_gate));
    m->pit_gate[0]=1; m->pit_gate[1]=1; m->pit_gate[2]=1;
    m->pit_count[0]=0; m->pit_reload[0]=0; m->pit_phase=0;
    m->kbd_command_byte=0x04; m->kbd_enabled=1; m->kbd_mouse_enabled=0;
    m->kbd_output_full=0; m->kbd_input_full=0; m->kbd_expect_command=0; m->kbd_expect_mouse=0;
    m->cmos_index=0;
    memset(m->dma_addr,0,sizeof(m->dma_addr));
    memset(m->dma_count,0,sizeof(m->dma_count));
    memset(m->dma_page,0,sizeof(m->dma_page));
    memset(m->dma_mode,0,sizeof(m->dma_mode));
    m->dma_mask=0xFF; m->dma_flipflop=0;
    m->pci_addr=0; m->pci_addr_valid=0;
    devices_pci_init(m);
    devices_ide_init(m);
    devices_vga_init(m);
    devices_cmos_init(m);
    memset(m->lapic_regs,0,sizeof(m->lapic_regs));
    memset(m->ioapic_regs,0,sizeof(m->ioapic_regs));
    memset(m->hpet_regs,0,sizeof(m->hpet_regs));
    m->lapic_timer_pending=0;
    m->time_initialized=0;
    m->last_time_ns=now_ns();
    m->emulated_ns=0;
}
void devices_reset(PcMachine* m){
    devices_init(m);
    devices_acpi_install(m);
    /* Re-init IDE after ACPI (plan already loaded) */
    devices_ide_init(m);
}

/* ---------- Tick ---------- */
void devices_tick(PcMachine* m, uint64_t instructions){
    /* PIT: 1193182 Hz, CPU 100 MHz => tick every ~84 cycles. Accumulate phase. */
    m->pit_phase += instructions * 1193182ULL;
    while(m->pit_phase >= 100000000ULL){
        m->pit_phase -= 100000000ULL;
        pit_tick_channel(m,0);
        pit_tick_channel(m,1);
        pit_tick_channel(m,2);
    }
    /* LAPIC timer: decrement if enabled */
    if(m->lapic_timer_count && (m->lapic_timer_mode & 0x10)){
        /* periodic */
        if(m->lapic_timer_count > instructions) m->lapic_timer_count -= (uint32_t)instructions;
        else {
            m->lapic_timer_count = m->lapic_timer_init;
            m->lapic_timer_pending = 1;
        }
    } else if(m->lapic_timer_count && !(m->lapic_timer_mode & 0x10)){
        if(m->lapic_timer_count > instructions) m->lapic_timer_count -= (uint32_t)instructions;
        else { m->lapic_timer_count=0; m->lapic_timer_pending=1; }
    }
    /* HPET counter */
    if(m->hpet_enabled){
        m->hpet_phase += instructions * 14318181ULL; /* 14.318 MHz / 100 MHz */
        while(m->hpet_phase >= 100000000ULL){ m->hpet_phase-=100000000ULL; m->hpet_counter_low++; }
    }
    /* RTC tick via BIOS ticks */
    /* Keyboard IRQ deassert if output buffer empty */
    if(!m->kbd_output_full) pic_lower(m,1);
}

/* ---------- Ports ---------- */
uint8_t devices_port_in(PcMachine* m, uint16_t port){
    if(port>=0x20 && port<=0x21) return pic_read(m,0,port==0x20);
    if(port>=0xA0 && port<=0xA1) return pic_read(m,1,port==0xA0);
    if(port>=0x40 && port<=0x43) return pit_read(m,port-0x40);
    if(port==0x60){
        if(m->kbd_output_full){
            m->kbd_output_full=0;
            pic_lower(m,1);
            return m->kbd_output;
        }
        return 0;
    }
    if(port==0x64) return kbd_status(m);
    if(port==0x70) return m->cmos_index;
    if(port==0x71) return cmos_read(m,m->cmos_index & 0x7F);
    if(port==0x92) return m->cpu.a20_enabled ? 0x02 : 0x00;
    if(port<=0x0F) return dma_read(m,port);
    if(port>=0x80 && port<=0x8F) return dma_read(m,port);
    if (port >= 0xCF8 && port <= 0xCFB) return (uint8_t)(m->pci_addr >> (8 * (port - 0xCF8)));
    if (port >= 0xCFC && port <= 0xCFF) return pci_read_config(m, (m->pci_addr & ~3u) + (port - 0xCFC));
    if((port>=0x1F0 && port<=0x1F7) || (port>=0x170 && port<=0x177) || port==0x3F6 || port==0x376) return ide_port_in(m,port);
    if(port>=0x3C0 && port<=0x3DF) return vga_port_in(m,port);
    if(port==0x61) return 0;
    if(port>=0x3F0 && port<=0x3F7){ /* FDC */
        if(port==0x3F4) return m->fdc_status;
        if(port==0x3F5) return 0;
        return 0;
    }
    if(port==0x3FD) return 0x20;
    if(port==0x3F8) return 0;
    if(port==OPENXISO_FIRMWARE_PORT) return 0x10;
    if(port==OPENXISO_FIRMWARE_POST_PORT) return 0x01;
    return 0xFF;
}
void devices_port_out(PcMachine* m, uint16_t port, uint8_t value){
    if(port==0x20) { pic_write(m,0,1,value); return; }
    if(port==0x21) { pic_write(m,0,0,value); return; }
    if(port==0xA0) { pic_write(m,1,1,value); return; }
    if(port==0xA1) { pic_write(m,1,0,value); return; }
    if(port>=0x40 && port<=0x42) { pit_write(m,port-0x40,value); return; }
    if(port==0x43) { pit_command(m,value); return; }
    if (port == 0x60) {
        if (m->kbd_expect_command == 1) m->kbd_command_byte = value;
        else if (m->kbd_expect_command == 2) m->cpu.a20_enabled = !!(value & 2);
        else m->kbd_input_buffer = value;
        m->kbd_expect_command = 0;
        m->kbd_expect_mouse = 0;
        m->kbd_input_full = 0;
        return;
    }
    if (port == 0x64) {
        switch (value) {
            case 0x20: m->kbd_output = m->kbd_command_byte; m->kbd_output_full = 1; break;
            case 0x60: m->kbd_expect_command = 1; break;
            case 0xD0: m->kbd_output = 1 | (m->cpu.a20_enabled ? 2 : 0); m->kbd_output_full = 1; break;
            case 0xD1: m->kbd_expect_command = 2; break;
            case 0xDD: m->cpu.a20_enabled = 0; break;
            case 0xDF: m->cpu.a20_enabled = 1; break;
            case 0xAD: m->kbd_enabled = 0; break;
            case 0xAE: m->kbd_enabled = 1; break;
            case 0xA7: m->kbd_mouse_enabled = 0; break;
            case 0xA8: m->kbd_mouse_enabled = 1; break;
            case 0xA9: case 0xAB: m->kbd_output = 0; m->kbd_output_full = 1; break;
            case 0xAA: m->kbd_output = 0x55; m->kbd_output_full = 1; break;
            case 0xD4: m->kbd_expect_mouse = 1; break;
            default: break;
        }
        return;
    }
    if(port==0x70){ m->cmos_index=value; return; }
    if(port==0x71){ cmos_write(m,m->cmos_index & 0x7F,value); return; }
    if(port==0x92){ m->cpu.a20_enabled = (value & 0x02) ? 1 : 0; if(value & 0x01){ m->cpu.halted=1; } return; }
    if(port<=0x0F){ dma_write(m,port,value); return; }
    if(port>=0x80 && port<=0x8F){ dma_write(m,port,value); return; }
    if (port >= 0xCF8 && port <= 0xCFB) {
        unsigned shift = 8 * (port - 0xCF8);
        m->pci_addr = (m->pci_addr & ~(0xffu << shift)) | ((uint32_t)value << shift);
        return;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        pci_write_config(m, (m->pci_addr & ~3u) + (port - 0xCFC), value);
        return;
    }
    if((port>=0x1F0 && port<=0x1F7) || (port>=0x170 && port<=0x177) || port==0x3F6 || port==0x376){ ide_port_out(m,port,value); return; }
    if(port>=0x3C0 && port<=0x3DF){ vga_port_out(m,port,value); return; }
    if(port==0x61){ return; }
    if(port>=0x3F0 && port<=0x3F7){ return; }
    if(port==0x3F8){ if(value=='\n'||value=='\r'){ if(m->serial_length>0){ m->serial_line[m->serial_length]='\0'; legacy_bios_log(m,"COM1: %s",m->serial_line); m->serial_length=0; } } else if(m->serial_length+1<(int)sizeof(m->serial_line)) m->serial_line[m->serial_length++]=(char)value; return; }
    if(port==OPENXISO_FIRMWARE_PORT){ legacy_bios_interrupt(m,value); return; }
    if(port==OPENXISO_FIRMWARE_POST_PORT){ legacy_bios_post(m); return; }
}
