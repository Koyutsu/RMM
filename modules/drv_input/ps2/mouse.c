#include "drivers/ps2/mouse.h"
#include "drivers/ps2/keyboard.h"     /* inb/outb + PS2_* port constants */
#include "drivers/input/evdev.h"
#include "cpu/idt.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * mouse.c - PS/2 (i8042 aux) mouse driver feeding evdev
 *
 * i8042 aux protocol (stream mode, defaults 0xF6):
 *   byte0: [ y_ovf, x_ovf, y_sign, x_sign, 1, mid, right, left ]
 *   byte1: dx   byte2: dy     (two's complement, 9-bit with sign bits)
 *
 * Packets are re-synchronised on byte0 bit3 (always 1 in stream mode),
 * so one stolen/lost byte costs a single packet, the next valid byte0
 * realigns the stream.
 *
 * Init is executed with interrupts disabled so controller ACK bytes
 * cannot be stolen by the keyboard IRQ handler, every wait is bounded,
 * making the probe fail-safe on mouse-less machines.
 */

static bool mouse_on = false;

bool mouse_enabled(void) { return mouse_on; }

/* bounded controller waits */

#define AUX_SPIN_MAX 200000u

static bool wait_ibf_clear(void) {
    for (uint32_t i = 0; i < AUX_SPIN_MAX; i++)
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) return true;
    return false;
}

static bool wait_obf_set(void) {
    for (uint32_t i = 0; i < AUX_SPIN_MAX; i++)
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) return true;
    return false;
}

static void ps2_cmd(uint8_t cmd) {
    if (wait_ibf_clear()) outb(PS2_COMMAND_PORT, cmd);
}

static uint8_t ps2_read(void) {
    return inb(PS2_DATA_PORT);
}

/* Write to the AUX device (0xD4-prefixed command byte to port 0x60). */
static bool aux_write(uint8_t b) {
    if (!wait_ibf_clear()) return false;
    outb(PS2_COMMAND_PORT, 0xD4);
    if (!wait_ibf_clear()) return false;
    outb(PS2_DATA_PORT, b);
    return true;
}

/* Wait for the device's 0xFA ACK (ignoring any non-ACK byte). */
static bool aux_expect_ack(void) {
    for (uint32_t i = 0; i < AUX_SPIN_MAX; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)) continue;
        uint8_t b = inb(PS2_DATA_PORT);
        if (b == 0xFA) return true;
        if (b == 0xFE) return false;          /* resend request: give up */
    }
    return false;
}

/* IRQ12: stream packets -> evdev */

static uint8_t  pkt[3];
static int      pkt_idx = 0;

static void mouse_irq(void) {
    uint8_t st = inb(PS2_STATUS_PORT);
    if (!(st & PS2_STATUS_OUTPUT_FULL)) return;
    if (!(st & PS2_STATUS_AUX)) return;       /* not for us */

    uint8_t b = inb(PS2_DATA_PORT);

    if (pkt_idx == 0 && !(b & 0x08)) return;  /* bit3 sync lost: resync */

    pkt[pkt_idx++] = b;
    if (pkt_idx < 3) return;
    pkt_idx = 0;

    if (pkt[0] & 0xC0) return;                /* x/y overflow: drop packet */

    uint8_t buttons = (uint8_t)(pkt[0] & 0x07);
    int dx = (int)pkt[1];
    int dy = (int)pkt[2];
    if (pkt[0] & 0x10) dx -= 256;             /* sign-extend 9-bit values */
    if (pkt[0] & 0x20) dy -= 256;

    evdev_mouse_packet(dx, dy, buttons);
}

/* init */

bool mouse_init(void) {
    __asm__ volatile("cli");

    bool ok = false;
    uint8_t cfg = 0;

    /* 1. enable the aux clock (command 0xA8) */
    ps2_cmd(0xA8);

    /* 2. read config byte (0x20): enable IRQ12, enable mouse clock */
    ps2_cmd(0x20);
    if (wait_obf_set()) {
        cfg = ps2_read();
        uint8_t newcfg = (uint8_t)((cfg | 0x02) & ~0x20);
        ps2_cmd(0x60);
        if (wait_ibf_clear()) outb(PS2_DATA_PORT, newcfg);

        /* 3. mouse: set defaults (0xF6) + enable data reporting (0xF4) */
        if (aux_write(0xF6) && aux_expect_ack() &&
            aux_write(0xF4) && aux_expect_ack()) {
            ok = true;
        } else {
            /* probe failed: restore the original config byte */
            ps2_cmd(0x60);
            if (wait_ibf_clear()) outb(PS2_DATA_PORT, cfg);
        }
    }

    if (ok) {
        idt_register_irq(12, mouse_irq);
        pic_unmask(12);
        pkt_idx = 0;
        mouse_on = true;
    }

    __asm__ volatile("sti");
    return ok;
}
