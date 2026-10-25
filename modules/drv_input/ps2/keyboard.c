#include "drivers/ps2/keyboard.h"
#include "drivers/input/evdev.h"
#include "cpu/idt.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * PS/2 Set 1 scancode -> ASCII / KEY_* translation tables.
 *
 * Index = make-code (0x00-0x7F).
 * Values verified against the IBM Technical Reference Set-1 table.
 *
 * 0x00  (none)
 * 0x01  ESC
 * 0x02  1    0x03  2    0x04  3    0x05  4    0x06  5    0x07  6
 * 0x08  7    0x09  8    0x0A  9    0x0B  0    0x0C  -    0x0D  =
 * 0x0E  BS   0x0F  TAB
 * 0x10  q    0x11  w    0x12  e    0x13  r    0x14  t    0x15  y
 * 0x16  u    0x17  i    0x18  o    0x19  p    0x1A  [    0x1B  ]
 * 0x1C  ENTER  0x1D  LCTRL
 * 0x1E  a    0x1F  s    0x20  d    0x21  f    0x22  g    0x23  h
 * 0x24  j    0x25  k    0x26  l    0x27  ;    0x28  '    0x29  `
 * 0x2A  LSHIFT  0x2B  \
 * 0x2C  z    0x2D  x    0x2E  c    0x2F  v    0x30  b    0x31  n
 * 0x32  m    0x33  ,    0x34  .    0x35  /    0x36  RSHIFT
 * 0x37  KP*  0x38  LALT  0x39  space  0x3A  CAPS
 * 0x3B - 0x44  F1 - F10
 * 0x45  NUMLOCK  0x46  SCROLLLOCK
 * 0x47  KP7  0x48  KP8  0x49  KP9  0x4A  KP-
 * 0x4B  KP4  0x4C  KP5  0x4D  KP6  0x4E  KP+
 * 0x4F  KP1  0x50  KP2  0x51  KP3  0x52  KP0  0x53  KP.
 * 0x57  F11  0x58  F12
 */

static const uint16_t sc_normal[128] = {
/* 00 */ 0,      '\x1B', '1',    '2',    '3',    '4',    '5',    '6',
/* 08 */ '7',    '8',    '9',    '0',    '-',    '=',    '\b',   '\t',
/* 10 */ 'q',    'w',    'e',    'r',    't',    'y',    'u',    'i',
/* 18 */ 'o',    'p',    '[',    ']',    '\n',   0,      'a',    's',
/* 20 */ 'd',    'f',    'g',    'h',    'j',    'k',    'l',    ';',
/* 28 */ '\'',   '`',    0,      '\\',   'z',    'x',    'c',    'v',
/* 30 */ 'b',    'n',    'm',    ',',    '.',    '/',    0,      '*',
/* 38 */ 0,      ' ',    0,      KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
/* 40 */ KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,0,      0,      KEY_HOME,
/* 48 */ KEY_UP, KEY_PGUP,'-',  KEY_LEFT,'5',  KEY_RIGHT,'+',   KEY_END,
/* 50 */ KEY_DOWN,KEY_PGDN,KEY_INS,KEY_DEL,0,   0,      0,      KEY_F11,
/* 58 */ KEY_F12,0,      0,      0,      0,      0,      0,      0,
/* 60 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 68 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 70 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 78 */ 0,      0,      0,      0,      0,      0,      0,      0,
};

static const uint16_t sc_shifted[128] = {
/* 00 */ 0,      '\x1B', '!',    '@',    '#',    '$',    '%',    '^',
/* 08 */ '&',    '*',    '(',    ')',    '_',    '+',    '\b',   '\t',
/* 10 */ 'Q',    'W',    'E',    'R',    'T',    'Y',    'U',    'I',
/* 18 */ 'O',    'P',    '{',    '}',    '\n',   0,      'A',    'S',
/* 20 */ 'D',    'F',    'G',    'H',    'J',    'K',    'L',    ':',
/* 28 */ '"',    '~',    0,      '|',    'Z',    'X',    'C',    'V',
/* 30 */ 'B',    'N',    'M',    '<',    '>',    '?',    0,      '*',
/* 38 */ 0,      ' ',    0,      KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
/* 40 */ KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,0,      0,      KEY_HOME,
/* 48 */ KEY_UP, KEY_PGUP,'_',  KEY_LEFT,'%',  KEY_RIGHT,'+',   KEY_END,
/* 50 */ KEY_DOWN,KEY_PGDN,KEY_INS,KEY_DEL,0,   0,      0,      KEY_F11,
/* 58 */ KEY_F12,0,      0,      0,      0,      0,      0,      0,
/* 60 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 68 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 70 */ 0,      0,      0,      0,      0,      0,      0,      0,
/* 78 */ 0,      0,      0,      0,      0,      0,      0,      0,
};

/*
 * Extended (E0-prefixed) make-codes -> KEY_*
 * These are the "grey" keys: arrows, ins, del, home, end, pgup, pgdn,
 * right ctrl, right alt, etc.
 */
static uint16_t sc_ext(uint8_t sc) {
    switch (sc) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    case 0x52: return KEY_INS;
    case 0x53: return KEY_DEL;
    case 0x1C: return '\n';       /* KP Enter */
    case 0x35: return '/';        /* KP /     */
    default:   return KEY_NONE;
    }
}

/* Modifier make-codes */
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_LCTRL   0x1D
#define SC_LALT    0x38
#define SC_CAPS    0x3A

/*
 * (evdev): Set-1 make-code -> evdev wire code for the event
 * stream (the numeric codes programs expect there are the USB-HID
 * usage IDs).  Used only for the evdev event stream, the console/
 * ASCII tables above are untouched.  Keypad keys carry their own KP
 * codes, the editing/arrow keys exist only in the E0-extended form.
 */
static const uint16_t sc_lnx[128] = {
/* 00 */ 0,      LKEY_ESC, LKEY_1,  LKEY_2,  LKEY_3,  LKEY_4,  LKEY_5,  LKEY_6,
/* 08 */ LKEY_7,LKEY_8,   LKEY_9,  LKEY_0,  LKEY_MINUS, LKEY_EQUAL, LKEY_BACKSPACE, LKEY_TAB,
/* 10 */ LKEY_Q, LKEY_W,   LKEY_E,  LKEY_R,  LKEY_T,  LKEY_Y,  LKEY_U,  LKEY_I,
/* 18 */ LKEY_O, LKEY_P,   LKEY_LEFTBRACE, LKEY_RIGHTBRACE, LKEY_ENTER, 0, LKEY_A, LKEY_S,
/* 20 */ LKEY_D, LKEY_F,   LKEY_G,  LKEY_H,  LKEY_J,  LKEY_K,  LKEY_L,  LKEY_SEMICOLON,
/* 28 */ LKEY_APOSTROPHE, LKEY_GRAVE, 0, LKEY_BACKSLASH, LKEY_Z, LKEY_X, LKEY_C, LKEY_V,
/* 30 */ LKEY_B, LKEY_N,   LKEY_M,  LKEY_COMMA, LKEY_DOT, LKEY_SLASH, 0, LKEY_KPASTERISK,
/* 38 */ LKEY_LEFTALT, LKEY_SPACE, LKEY_CAPSLOCK, LKEY_F1, LKEY_F2, LKEY_F3, LKEY_F4, LKEY_F5,
/* 40 */ LKEY_F6, LKEY_F7, LKEY_F8, LKEY_F9, LKEY_F10, LKEY_NUMLOCK, LKEY_SCROLLLOCK, LKEY_KP7,
/* 48 */ LKEY_KP8, LKEY_KP9, LKEY_KPMINUS, LKEY_KP4, LKEY_KP5, LKEY_KP6, LKEY_KPPLUS, LKEY_KP1,
/* 50 */ LKEY_KP2, LKEY_KP3, LKEY_KP0, LKEY_KPDOT, 0, 0, LKEY_102ND, LKEY_F11,
/* 58 */ LKEY_F12, 0, 0, 0, 0, 0, 0, 0,
/* 60..7f */ 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
};

/* E0-prefixed make-codes -> evdev wire codes (grey keys + modifiers) */
static uint16_t lnx_ext(uint8_t sc) {
    switch (sc) {
    case 0x1C: return LKEY_KPENTER;
    case 0x1D: return LKEY_RIGHTCTRL;
    case 0x35: return LKEY_KPSLASH;
    case 0x37: return LKEY_SYSRQ;
    case 0x38: return LKEY_RIGHTALT;
    case 0x47: return LKEY_HOME;
    case 0x48: return LKEY_UP;
    case 0x49: return LKEY_PAGEUP;
    case 0x4B: return LKEY_LEFT;
    case 0x4D: return LKEY_RIGHT;
    case 0x4F: return LKEY_END;
    case 0x50: return LKEY_DOWN;
    case 0x51: return LKEY_PAGEDOWN;
    case 0x52: return LKEY_INSERT;
    case 0x53: return LKEY_DELETE;
    case 0x5B: return LKEY_LEFTMETA;
    case 0x5C: return LKEY_RIGHTMETA;
    case 0x5D: return LKEY_COMPOSE;
    /* PrintScreen synthesises E0 2A / E0 B7 around E0 37 - ignore both */
    case 0x2A: case 0xB7: case 0xAA: case 0xB6: return LKEY_RESERVED;
    default:   return LKEY_RESERVED;
    }
}

/* Raw evdev byte from the AUX (mouse) port, leave it for IRQ12 when the
 * PS/2 mouse driver owns the port, drain it only when it does not, so a
 * dead-mouse machine cannot clog the controller buffer. */
extern bool mouse_enabled(void);

/* Ring buffer */
#define KB_BUF 256
static volatile uint16_t kbuf[KB_BUF];
static volatile int      khead = 0;
static volatile int      ktail = 0;

static void kpush(uint16_t k) {
    int n = (khead + 1) % KB_BUF;
    if (n != ktail) { kbuf[khead] = k; khead = n; }
}
static uint16_t kpop(void) {
    if (khead == ktail) return KEY_NONE;
    uint16_t k = kbuf[ktail];
    ktail = (ktail + 1) % KB_BUF;
    return k;
}

/* Modifier state */
static volatile bool mod_lshift = false;
static volatile bool mod_rshift = false;
static volatile bool mod_ctrl   = false;
static volatile bool mod_alt    = false;
static volatile bool mod_caps   = false;
static volatile bool mod_ext    = false;   /* saw 0xE0 prefix */
static volatile int  e1_skip    = 0;       /* bytes left in E1 (Pause) seq */

static void kb_irq(void) {
    /* validate the byte before consuming it.
     *  - no output-buffer byte  -> spurious IRQ, do not read port 0x60
     *  - AUX bit set (mouse)    -> leave it for the mouse IRQ
     *                               handler when the PS/2 mouse driver is
     *                               active, drain otherwise (no driver
     *                               would ever read it) so the buffer
     *                               cannot clog.
     *  - 0xFA ACK / 0xFE resend -> controller responses, not key data   */
    uint8_t st = inb(PS2_STATUS_PORT);
    if (!(st & PS2_STATUS_OUTPUT_FULL)) return;
    if (st & PS2_STATUS_AUX) {
        if (!mouse_enabled()) (void)inb(PS2_DATA_PORT);
        return;
    }

    uint8_t sc = inb(PS2_DATA_PORT);

    if (sc == 0xFA || sc == 0xFE) return;      /* ACK / resend */

    /* E0 prefix - next byte is an extended scancode */
    if (sc == 0xE0) { mod_ext = true; return; }
    /* E1 prefix (Pause key = E1 14 77 E1 F0 14 F0 77) - swallow the
     * whole fixed-length sequence so no stray bytes leak through, then
     * report a synthetic evdev press+release pair.           */
    if (sc == 0xE1) { e1_skip = 7; return; }
    if (e1_skip > 0) {
        e1_skip--;
        if (e1_skip == 0) evdev_kbd_synthetic(LKEY_PAUSE);
        return;
    }

    bool is_break = (sc & 0x80) != 0;
    uint8_t make  = sc & 0x7F;

    /* Handle extended key */
    if (mod_ext) {
        mod_ext = false;
        /* Right Ctrl / Right Alt modifier tracking */
        if (make == 0x1D) { mod_ctrl = !is_break; evdev_kbd_event(LKEY_RIGHTCTRL, is_break); return; }
        if (make == 0x38) { mod_alt  = !is_break; evdev_kbd_event(LKEY_RIGHTALT, is_break); return; }
        if (!is_break) {
            uint16_t key = sc_ext(make);
            if (key != KEY_NONE) kpush(key);
        }
        /* evdev sees both make and break of every extended key */
        evdev_kbd_event(lnx_ext(make), is_break);
        return;
    }

    /* Modifier keys */
    if (make == SC_LSHIFT) { mod_lshift = !is_break; evdev_kbd_event(LKEY_LEFTSHIFT, is_break); return; }
    if (make == SC_RSHIFT) { mod_rshift = !is_break; evdev_kbd_event(LKEY_RIGHTSHIFT, is_break); return; }
    if (make == SC_LCTRL)  { mod_ctrl   = !is_break; evdev_kbd_event(LKEY_LEFTCTRL, is_break); return; }
    if (make == SC_LALT)   { mod_alt    = !is_break; evdev_kbd_event(LKEY_LEFTALT, is_break); return; }
    if (make == SC_CAPS && !is_break) { mod_caps = !mod_caps; evdev_kbd_event(LKEY_CAPSLOCK, false); evdev_kbd_event(LKEY_CAPSLOCK, true); return; }

    /* Only process make codes from here */
    if (is_break) {
        /* plain-key BREAK still feeds evdev (console ignores it) */
        if (make < 128 && sc_lnx[make]) evdev_kbd_event(sc_lnx[make], true);
        return;
    }
    if (make >= 128) return;

    /* evdev gets the raw key press (unshifted wire code) before
     * any console translation, the shift/ctrl logic below is console-only
     * semantics and has early returns for F-keys/editing keys.           */
    if (sc_lnx[make]) evdev_kbd_event(sc_lnx[make], false);

    bool shift = mod_lshift || mod_rshift;

    uint16_t norm = sc_normal[make];
    uint16_t shft = sc_shifted[make];

    /* Special/function keys: pass through regardless of shift */
    if (norm > 0x7F) { kpush(norm); return; }

    uint16_t key;
    /* Letters: caps XOR shift determines case */
    if (norm >= 'a' && norm <= 'z') {
        bool upper = mod_caps ^ shift;
        key = upper ? shft : norm;
    } else {
        key = shift ? shft : norm;
    }

    if (key == 0) return;

    /* Ctrl combos: Ctrl+A..Z -> 0x01..0x1A */
    if (mod_ctrl) {
        if (key >= 'a' && key <= 'z') key = (uint16_t)(key - 'a' + 1);
        else if (key >= 'A' && key <= 'Z') key = (uint16_t)(key - 'A' + 1);
    }

    kpush(key);
}

/* Public API */
void kb_init(void) {
    /* Drain stale bytes */
    while (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL)
        inb(PS2_DATA_PORT);

    mod_ext = mod_lshift = mod_rshift = false;
    mod_ctrl = mod_alt = mod_caps = false;
    khead = ktail = 0;

    idt_register_irq(1, kb_irq);
    pic_unmask(1);

    /* Enable key scanning */
    outb(PS2_DATA_PORT, 0xF4);
}

int  kb_getchar_nb(void) { return (int)kpop(); }
bool kb_has_char(void)   { return khead != ktail; }

int kb_getchar(void) {
    int c;
    do { __asm__ volatile("hlt"); c = kb_getchar_nb(); } while (!c);
    return c;
}

kb_mods_t kb_modifiers(void) {
    return (kb_mods_t){
        .shift = mod_lshift || mod_rshift,
        .ctrl  = mod_ctrl,
        .alt   = mod_alt,
        .caps  = mod_caps,
        .num   = false,
    };
}
