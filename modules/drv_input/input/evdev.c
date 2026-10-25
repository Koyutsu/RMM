#include "drivers/input/evdev.h"
#include "drivers/timer/pit.h"
#include "exec/sched.h"
#include "exec/syscall_table.h"      /* errno values (ENODEV, EINVAL, ...) */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * evdev.c - input-event device core over the evdev wire ABI
 *
 * Two static device slots (keyboard, mouse), each with an SPSC ring of
 * struct input_event.  Producers run at IRQ level (i8042 IRQ1/IRQ12);
 * the single consumer is a userland reader through /dev/input/eventN -
 * the same cooperative single-reader contract as the rest of the VFS.
 * overflow drops the oldest events and reports SYN_DROPPED once so a
 * reader can resynchronise its state.
 *
 * Blocking read() parks the task through the BORE scheduler continuation
 * (sched_kernel_park), exactly like the console tty read path, so an
 * evtest(1) or compositor blocks without burning CPU.
 */

/* per-device state */

#define EVQ_ORDER 7                       /* 128 events per device        */
#define EVQ_SIZE  (1u << EVQ_ORDER)
#define EVQ_MASK  (EVQ_SIZE - 1u)

typedef struct {
    /* SPSC ring: head written by the IRQ producer, tail by the reader */
    volatile uint32_t   head, tail;
    bool                dropped;          /* overflow -> emit SYN_DROPPED  */
    linput_event_t      q[EVQ_SIZE];

    /* identity reported via EVIOCG* */
    const char         *name;
    const char         *phys;
    uint16_t            bustype, vendor, product, version;

    /* capability bits (EVIOCGBIT): key_bits[0] = supported-TYPES bitmap;
     * key_bits[type] = supported-code bitmap for that type (key needs 96B) */
    uint8_t             key_bits[LEV_MAX + 1][96];

    /* current key/button state (EVIOCGKEY): bit = code index            */
    uint8_t             keystate[96];     /* KEY_MAX=0x2ff -> 768 bits = 96 B */
} evdev_dev_t;

#define KEYSTATE_BYTES 96   /* keep in sync with keystate above */

static evdev_dev_t evdev_devs[LEVDEV_COUNT];
static bool        evdev_ready = false;

/* helpers */

static uint64_t ev_now_us(void) {
    uint64_t ms = pit_ticks();
    return ms * 1000ULL;
}

static void set_bit_u8(uint8_t *map, unsigned bit) {
    map[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void ev_push(evdev_dev_t *d, uint16_t type, uint16_t code, int32_t value) {
    uint64_t now = ev_now_us();
    uint32_t next = (d->head + 1) & EVQ_MASK;
    if (next == d->tail) {
        /* ring full: mark drop, reader emits SYN_DROPPED before resync */
        d->dropped = true;
        return;
    }
    linput_event_t *e = &d->q[d->head];
    e->tv_sec  = (int64_t)(now / 1000000ULL);
    e->tv_usec = (int64_t)(now % 1000000ULL);
    e->type    = type;
    e->code    = code;
    e->value   = value;
    d->head    = next;
}

static void ev_push_syn(evdev_dev_t *d) {
    ev_push(d, LEV_SYN, LSYN_REPORT, 0);
}

/* producers */

void evdev_init(void) {
    for (int i = 0; i < LEVDEV_COUNT; i++) {
        evdev_dev_t *d = &evdev_devs[i];
        d->head = d->tail = 0;
        d->dropped = false;
        for (unsigned t = 0; t <= LEV_MAX; t++)
            for (int b = 0; b < 96; b++) d->key_bits[t][b] = 0;
        for (int b = 0; b < KEYSTATE_BYTES; b++) d->keystate[b] = 0;
    }

    evdev_dev_t *k = &evdev_devs[LEVDEV_KBD];
    k->name    = "AT Translated Set 2 keyboard";
    k->phys    = "isa0060/serio0/input0";
    k->bustype = LBUS_I8042;  k->vendor = 0x4c55; k->product = 0x0001; k->version = 0x0100;
    /* key_bits[0] = supported event-TYPES bitmap: SYN, key, MSC */
    k->key_bits[0][0] = (uint8_t)((1u << LEV_SYN) | (1u << LEV_KEY) | (1u << LEV_MSC));
    k->key_bits[LEV_MSC][0] = (uint8_t)(1u << LMSC_SCAN);   /* MSC codes bitmap */
    /* keys we can produce (bits set lazily would be fine, be explicit) */
    {
        static const uint16_t keys[] = {
            LKEY_ESC, LKEY_1, LKEY_2, LKEY_3, LKEY_4, LKEY_5, LKEY_6, LKEY_7,
            LKEY_8, LKEY_9, LKEY_0, LKEY_MINUS, LKEY_EQUAL, LKEY_BACKSPACE,
            LKEY_TAB, LKEY_Q, LKEY_W, LKEY_E, LKEY_R, LKEY_T, LKEY_Y, LKEY_U,
            LKEY_I, LKEY_O, LKEY_P, LKEY_LEFTBRACE, LKEY_RIGHTBRACE, LKEY_ENTER,
            LKEY_LEFTCTRL, LKEY_A, LKEY_S, LKEY_D, LKEY_F, LKEY_G, LKEY_H,
            LKEY_J, LKEY_K, LKEY_L, LKEY_SEMICOLON, LKEY_APOSTROPHE, LKEY_GRAVE,
            LKEY_LEFTSHIFT, LKEY_BACKSLASH, LKEY_Z, LKEY_X, LKEY_C, LKEY_V,
            LKEY_B, LKEY_N, LKEY_M, LKEY_COMMA, LKEY_DOT, LKEY_SLASH,
            LKEY_RIGHTSHIFT, LKEY_KPASTERISK, LKEY_LEFTALT, LKEY_SPACE,
            LKEY_CAPSLOCK, LKEY_F1, LKEY_F2, LKEY_F3, LKEY_F4, LKEY_F5,
            LKEY_F6, LKEY_F7, LKEY_F8, LKEY_F9, LKEY_F10, LKEY_NUMLOCK,
            LKEY_SCROLLLOCK, LKEY_KP7, LKEY_KP8, LKEY_KP9, LKEY_KPMINUS,
            LKEY_KP4, LKEY_KP5, LKEY_KP6, LKEY_KPPLUS, LKEY_KP1, LKEY_KP2,
            LKEY_KP3, LKEY_KP0, LKEY_KPDOT, LKEY_F11, LKEY_F12, LKEY_KPENTER,
            LKEY_RIGHTCTRL, LKEY_KPSLASH, LKEY_SYSRQ, LKEY_RIGHTALT, LKEY_HOME,
            LKEY_UP, LKEY_PAGEUP, LKEY_LEFT, LKEY_RIGHT, LKEY_END, LKEY_DOWN,
            LKEY_PAGEDOWN, LKEY_INSERT, LKEY_DELETE, LKEY_PAUSE,
            LKEY_LEFTMETA, LKEY_RIGHTMETA, LKEY_COMPOSE,
        };
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            set_bit_u8(k->key_bits[LEV_KEY], keys[i]);
    }

    evdev_dev_t *m = &evdev_devs[LEVDEV_MOUSE];
    m->name    = "PS/2 Generic Mouse";
    m->phys    = "isa0060/serio1/input0";
    m->bustype = LBUS_I8042;  m->vendor = 0x4c55; m->product = 0x0002; m->version = 0x0100;
    /* key_bits[0] = supported event-TYPES bitmap: SYN, key, REL */
    m->key_bits[0][0] = (uint8_t)((1u << LEV_SYN) | (1u << LEV_KEY) | (1u << LEV_REL));
    m->key_bits[LEV_REL][0] = (uint8_t)((1u << LREL_X) | (1u << LREL_Y));

    /* mouse button capability bits live in the EV_KEY code bitmap */
    set_bit_u8(m->key_bits[LEV_KEY], LBTN_LEFT);
    set_bit_u8(m->key_bits[LEV_KEY], LBTN_RIGHT);
    set_bit_u8(m->key_bits[LEV_KEY], LBTN_MIDDLE);

    evdev_ready = true;
}

void evdev_kbd_event(uint16_t lk_code, bool release) {
    if (!evdev_ready || lk_code == LKEY_RESERVED) return;
    evdev_dev_t *d = &evdev_devs[LEVDEV_KBD];

    /* maintain EVIOCGKEY state */
    if ((lk_code >> 3) < KEYSTATE_BYTES) {
        if (release) d->keystate[lk_code >> 3] &= (uint8_t)~(1u << (lk_code & 7));
        else         d->keystate[lk_code >> 3] |= (uint8_t)(1u << (lk_code & 7));
    }

    ev_push(d, LEV_KEY, lk_code, release ? 0 : 1);
    ev_push_syn(d);
}

/* Press+release pair (e.g. Pause's multi-byte E1 sequence). */
void evdev_kbd_synthetic(uint16_t lk_code) {
    evdev_kbd_event(lk_code, false);
    evdev_kbd_event(lk_code, true);
}

void evdev_mouse_packet(int dx, int dy, uint8_t buttons) {
    if (!evdev_ready) return;
    evdev_dev_t *d = &evdev_devs[LEVDEV_MOUSE];

    static uint8_t prev = 0;
    static bool    have_prev = false;

    /* buttons first (like the PS/2 protocol order most stacks emit) */
    static const uint16_t bcode[3] = { LBTN_LEFT, LBTN_RIGHT, LBTN_MIDDLE };
    for (int b = 0; b < 3; b++) {
        bool now_down = (buttons >> b) & 1;
        bool was_down = have_prev && ((prev >> b) & 1);
        if (now_down != was_down) {
            uint16_t code = (uint16_t)bcode[b];
            if ((code >> 3) < KEYSTATE_BYTES) {
                if (now_down) d->keystate[code >> 3] |= (uint8_t)(1u << (code & 7));
                else          d->keystate[code >> 3] &= (uint8_t)~(1u << (code & 7));
            }
            ev_push(d, LEV_KEY, code, now_down ? 1 : 0);
        }
    }
    prev = buttons;
    have_prev = true;

    ev_push(d, LEV_REL, LREL_X, dx);
    ev_push(d, LEV_REL, LREL_Y, dy);
    ev_push_syn(d);
}

/* consumer side */

bool evdev_mouse_present(void) {
    extern bool mouse_enabled(void);          /* drivers/ps2/mouse.c */
    return mouse_enabled();
}

bool evdev_has_event(int idx) {
    if (!evdev_ready || idx < 0 || idx >= LEVDEV_COUNT) return false;
    evdev_dev_t *d = &evdev_devs[idx];
    if (d->head != d->tail) return true;
    return false;
}

int evdev_read(int idx, void *buf, size_t len) {
    if (!evdev_ready || idx < 0 || idx >= LEVDEV_COUNT) return -(int)ENODEV;
    evdev_dev_t *d = &evdev_devs[idx];

    /* Blocking read: park until at least one event is queued. */
    for (;;) {
        if (evdev_has_event(idx)) break;
        if (sched_kernel_park(BORE_EVENT_POLL_MS) != 0)
            __asm__ volatile("sti; hlt; cli");
    }

    size_t want = len / sizeof(linput_event_t);
    if (want == 0) return -(int)EINVAL;   /* not a whole event */
    if (want > EVQ_SIZE) want = EVQ_SIZE;

    size_t got = 0;
    linput_event_t *out = (linput_event_t *)buf;
    while (got < want && d->head != d->tail) {
        if (d->dropped) {
            /* synthesize the overflow marker once */
            uint64_t now = ev_now_us();
            out[got].tv_sec  = (int64_t)(now / 1000000ULL);
            out[got].tv_usec = (int64_t)(now % 1000000ULL);
            out[got].type    = LEV_SYN;
            out[got].code    = LSYN_DROPPED;
            out[got].value   = 1;
            got++;
            d->dropped = false;
            if (got >= want) break;
        }
        out[got] = d->q[d->tail];
        d->tail  = (d->tail + 1) & EVQ_MASK;
        got++;
    }
    return (int)(got * sizeof(linput_event_t));
}

/* EVIOCG* ioctls
 * x86-64 _IOC layout: dir(2)<<30 | size(14)<<16 | type(8)<<8 | nr(8)
 * _IOC_NONE=0, _IOC_WRITE=1, _IOC_READ=2.  'E' = 0x45.                 */
#define IOC_NRBITS   8
#define IOC_TYPEBITS 8
#define IOC_SIZEBITS 14
#define IOC_NR(nr)     ((nr) & 0xff)
#define IOC_TYPE(nr)   (((nr) >> IOC_NRBITS) & 0xff)
#define IOC_SIZE(nr)   (((nr) >> 16) & 0x3fff)
#define IOC_DIR(nr)    ((nr) >> 30)
#define IOC_READ       2u

#define EVIOC_BASE_TYPE 0x45

static long ev_copy_out(uint64_t arg, const void *src, size_t n) {
    if (!arg) return -(long)EFAULT;
    uint8_t *dst = (uint8_t *)(uintptr_t)arg;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) dst[i] = s[i];
    return 0;
}

static long ev_copy_out_name(uint64_t arg, size_t size, const char *s) {
    if (!arg || size == 0) return 0;
    size_t n = 0;
    while (s[n]) n++;
    if (n > size) n = size;                   /* truncate, NUL-pad */
    uint8_t zero[256];
    uint8_t *dst = (uint8_t *)(uintptr_t)arg;
    long r = ev_copy_out(arg, s, n);
    if (r != 0) return r;
    size_t pad = (size > n) ? size - n : 0;
    if (pad > sizeof(zero)) pad = sizeof(zero);
    for (size_t i = 0; i < pad; i++) zero[i] = 0;
    return ev_copy_out(arg + n, zero, pad);
}

long evdev_ioctl(int idx, uint64_t req, uint64_t arg) {
    if (!evdev_ready || idx < 0 || idx >= LEVDEV_COUNT) return -(long)ENODEV;
    evdev_dev_t *d = &evdev_devs[idx];

    if (IOC_TYPE(req) != EVIOC_BASE_TYPE) return -(long)ENOTTY;

    unsigned nr   = IOC_NR(req);
    size_t  size  = IOC_SIZE(req);

    switch (nr) {
    case 0x01: {                              /* EVIOCGVERSION */
        if (size < 4) return -(long)EINVAL;
        int32_t v = LEV_VERSION;
        return ev_copy_out(arg, &v, 4);
    }
    case 0x02: {                              /* EVIOCGID */
        if (size == 0) return -(long)EINVAL;
        linput_id_t id = { d->bustype, d->vendor, d->product, d->version };
        size_t n = (size < sizeof(id)) ? size : sizeof(id);
        return ev_copy_out(arg, &id, n);      /* size is advisory */
    }
    case 0x06:                                /* EVIOCGNAME(len) */
        return ev_copy_out_name(arg, size ? size : 256, d->name);
    case 0x07:                                /* EVIOCGPHYS(len) */
        return ev_copy_out_name(arg, size ? size : 256, d->phys);
    case 0x08:                                /* EVIOCGUNIQ(len) */
        return ev_copy_out_name(arg, size ? size : 256, "");
    case 0x09: {                              /* EVIOCGPROP(len) */
        size_t n = size ? size : 4;
        uint8_t zero[64];
        for (size_t i = 0; i < n && i < sizeof(zero); i++) zero[i] = 0;
        return ev_copy_out(arg, zero, (n < sizeof(zero)) ? n : sizeof(zero));
    }
    case 0x18: {                              /* EVIOCGKEY(len) */
        size_t n = size ? size : KEYSTATE_BYTES;
        if (n > KEYSTATE_BYTES) n = KEYSTATE_BYTES;
        uint8_t out[KEYSTATE_BYTES];
        for (size_t i = 0; i < KEYSTATE_BYTES; i++) out[i] = d->keystate[i];
        return ev_copy_out(arg, out, n);
    }
    default:
        break;
    }

    if (nr >= 0x20 && nr <= 0x20 + LEV_MAX) { /* EVIOCGBIT(ev, len) */
        unsigned ev = nr - 0x20;
        if (ev > LEV_MAX) return -(long)EINVAL;
        size_t n = size ? size : 96;
        if (n > 96) n = 96;                   /* largest bitmap is key (96 B) */
        return ev_copy_out(arg, d->key_bits[ev], n);
    }

    if (nr == 0x90)                           /* EVIOCGRAB - accept, no-op */
        return 0;

    return -(long)ENOTTY;
}
