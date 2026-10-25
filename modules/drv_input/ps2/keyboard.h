#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Special key codes returned by kb_getchar (values > 0x7F) */
#define KEY_UP       0x100
#define KEY_DOWN     0x101
#define KEY_LEFT     0x102
#define KEY_RIGHT    0x103
#define KEY_HOME     0x104
#define KEY_END      0x105
#define KEY_PGUP     0x106
#define KEY_PGDN     0x107
#define KEY_DEL      0x108
#define KEY_INS      0x109
#define KEY_F1       0x111
#define KEY_F2       0x112
#define KEY_F3       0x113
#define KEY_F4       0x114
#define KEY_F5       0x115
#define KEY_F6       0x116
#define KEY_F7       0x117
#define KEY_F8       0x118
#define KEY_F9       0x119
#define KEY_F10      0x11A
#define KEY_F11      0x11B
#define KEY_F12      0x11C
#define KEY_NONE     0x000

typedef struct {
    bool shift, ctrl, alt, caps, num;
} kb_mods_t;

void      kb_init(void);
int       kb_getchar_nb(void);
int       kb_getchar(void);
bool      kb_has_char(void);
kb_mods_t kb_modifiers(void);

/* Low-level port helpers */
static inline uint8_t inb(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v;
}
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));
}

#define PS2_DATA_PORT          0x60
#define PS2_STATUS_PORT        0x64
#define PS2_COMMAND_PORT       0x64
#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX         0x20   /* byte in buffer is from mouse */
