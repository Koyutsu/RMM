#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * evdev - Linux input-event ABI
 *
 * Exposes the kernel's PS/2 keyboard (and, when present, PS/2 mouse)
 * through /dev/input/eventN devices that are byte-compatible with the
 * evdev wire ABI (the event-stream interface input programs speak):
 *
 *   - struct input_event layout as used on x86-64
 *     ({ i64 tv_sec, i64 tv_usec, u16 type, u16 code, s32 value; } = 24 B)
 *   - EV_SYN/EV_KEY/EV_REL/EV_MSC types, SYN_REPORT protocol
 *   - KEY_* / BTN_* / REL_* numeric codes (USB-HID usage IDs, the
 *     values are what libinput/wlroots/compositors switch on)
 *   - EVIOCGVERSION / EVIOCGID / EVIOCGNAME / EVIOCGPHYS / EVIOCGUNIQ /
 *     EVIOCGPROP / EVIOCGKEY / EVIOCGBIT ioctls
 *   - poll()/epoll readiness (POLLIN when an event batch is queued)
 *
 * Devices are additive to the console path: the keyboard driver still
 * feeds the tty/serial console exactly as before, evdev observes the
 * same hardware in parallel (i8042 -> {console, event stream} split).
 *
 * Note on naming: the KEY_* numeric values live here under the
 * LK_* prefix because drivers/ps2/keyboard.h already defines an
 * unrelated internal KEY_* namespace for its ASCII-oriented ring.
 * The numbers are the wire values programs depend on.
 */

/* struct input_event (x86-64 wire layout, 24 bytes) */
typedef struct {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
} linput_event_t;            /* 24 bytes on x86-64 */

/* event types */
#define LEV_SYN              0x00
#define LEV_KEY              0x01
#define LEV_REL              0x02
#define LEV_ABS              0x03
#define LEV_MSC              0x04
#define LEV_SW               0x05
#define LEV_LED              0x11
#define LEV_SND              0x12
#define LEV_REP              0x14
#define LEV_FF               0x15
#define LEV_PWR              0x16
#define LEV_FF_STATUS        0x17
#define LEV_MAX              0x1f

/* SYN codes */
#define LSYN_REPORT          0
#define LSYN_CONFIG          1
#define LSYN_MT_REPORT       2
#define LSYN_DROPPED         3

/* REL axes */
#define LREL_X               0x00
#define LREL_Y               0x01
#define LREL_WHEEL           0x08

/* MSC codes */
#define LMSC_SCAN            0x04

/* KEY_* codes (wire values = USB-HID usage IDs) */
#define LKEY_RESERVED        0
#define LKEY_ESC             1
#define LKEY_1               2
#define LKEY_2               3
#define LKEY_3               4
#define LKEY_4               5
#define LKEY_5               6
#define LKEY_6               7
#define LKEY_7               8
#define LKEY_8               9
#define LKEY_9               10
#define LKEY_0               11
#define LKEY_MINUS           12
#define LKEY_EQUAL           13
#define LKEY_BACKSPACE       14
#define LKEY_TAB             15
#define LKEY_Q               16
#define LKEY_W               17
#define LKEY_E               18
#define LKEY_R               19
#define LKEY_T               20
#define LKEY_Y               21
#define LKEY_U               22
#define LKEY_I               23
#define LKEY_O               24
#define LKEY_P               25
#define LKEY_LEFTBRACE       26
#define LKEY_RIGHTBRACE      27
#define LKEY_ENTER           28
#define LKEY_LEFTCTRL        29
#define LKEY_A               30
#define LKEY_S               31
#define LKEY_D               32
#define LKEY_F               33
#define LKEY_G               34
#define LKEY_H               35
#define LKEY_J               36
#define LKEY_K               37
#define LKEY_L               38
#define LKEY_SEMICOLON       39
#define LKEY_APOSTROPHE      40
#define LKEY_GRAVE           41
#define LKEY_LEFTSHIFT       42
#define LKEY_BACKSLASH       43
#define LKEY_Z               44
#define LKEY_X               45
#define LKEY_C               46
#define LKEY_V               47
#define LKEY_B               48
#define LKEY_N               49
#define LKEY_M               50
#define LKEY_COMMA           51
#define LKEY_DOT             52
#define LKEY_SLASH           53
#define LKEY_RIGHTSHIFT      54
#define LKEY_KPASTERISK      55
#define LKEY_LEFTALT         56
#define LKEY_SPACE           57
#define LKEY_CAPSLOCK        58
#define LKEY_F1              59
#define LKEY_F2              60
#define LKEY_F3              61
#define LKEY_F4              62
#define LKEY_F5              63
#define LKEY_F6              64
#define LKEY_F7              65
#define LKEY_F8              66
#define LKEY_F9              67
#define LKEY_F10             68
#define LKEY_NUMLOCK         69
#define LKEY_SCROLLLOCK      70
#define LKEY_KP7             71
#define LKEY_KP8             72
#define LKEY_KP9             73
#define LKEY_KPMINUS         74
#define LKEY_KP4             75
#define LKEY_KP5             76
#define LKEY_KP6             77
#define LKEY_KPPLUS          78
#define LKEY_KP1             79
#define LKEY_KP2             80
#define LKEY_KP3             81
#define LKEY_KP0             82
#define LKEY_KPDOT           83
#define LKEY_ZENKAKUHANKAKU  85
#define LKEY_102ND           86
#define LKEY_F11             87
#define LKEY_F12             88
#define LKEY_RO              89
#define LKEY_KATAKANA        90
#define LKEY_HIRAGANA        91
#define LKEY_HENKAN          92
#define LKEY_KATAKANAHIRAGANA 93
#define LKEY_MUHENKAN        94
#define LKEY_KPJPCOMMA       95
#define LKEY_KPENTER         96
#define LKEY_RIGHTCTRL       97
#define LKEY_KPSLASH         98
#define LKEY_SYSRQ           99
#define LKEY_RIGHTALT        100
#define LKEY_LINEFEED        101
#define LKEY_HOME            102
#define LKEY_UP              103
#define LKEY_PAGEUP          104
#define LKEY_LEFT            105
#define LKEY_RIGHT           106
#define LKEY_END             107
#define LKEY_DOWN            108
#define LKEY_PAGEDOWN        109
#define LKEY_INSERT          110
#define LKEY_DELETE          111
#define LKEY_MACRO           112
#define LKEY_MUTE            113
#define LKEY_VOLUMEDOWN      114
#define LKEY_VOLUMEUP        115
#define LKEY_POWER           116
#define LKEY_KPEQUAL         117
#define LKEY_KPPLUSMINUS     118
#define LKEY_PAUSE           119
#define LKEY_SCALE           120
#define LKEY_KPCOMMA         121
#define LKEY_HANGEUL         122
#define LKEY_HANJA           123
#define LKEY_YEN             124
#define LKEY_LEFTMETA        125
#define LKEY_RIGHTMETA       126
#define LKEY_COMPOSE         127
#define LKEY_PLAYPAUSE       164
#define LKEY_STOP            166
#define LKEY_MAX             0x2ff

/* BTN_* (mouse) */
#define LBTN_LEFT            0x110
#define LBTN_RIGHT           0x111
#define LBTN_MIDDLE          0x112
#define LBTN_SIDE            0x113
#define LBTN_EXTRA           0x114

/* input_id (EVIOCGID) */
typedef struct {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} linput_id_t;

#define LBUS_PCI             0x04
#define LBUS_I8042           0x06

#define LEV_VERSION          0x010001   /* EV_VERSION the UAPI reports */

/* device indices */
#define LEVDEV_KBD           0          /* /dev/input/event0 - PS/2 keyboard */
#define LEVDEV_MOUSE         1          /* /dev/input/event1 - PS/2 mouse    */
#define LEVDEV_COUNT         2

/* lifecycle */
void evdev_init(void);

/* producers (IRQ context)
 * One logical batch per physical event: EV_KEY + EV_MSC(scan) + EV_SYN
 * for keys, evdev_mouse_packet() emits buttons + REL axes + EV_SYN.    */
void evdev_kbd_event(uint16_t lk_code, bool release);
void evdev_kbd_synthetic(uint16_t lk_code);     /* press+release pair      */
void evdev_mouse_packet(int dx, int dy, uint8_t buttons);

/* buttons bit mask for evdev_mouse_packet: bit0=left bit1=right bit2=mid */
#define LEVBTN_MASK_LEFT     0x01
#define LEVBTN_MASK_RIGHT    0x02
#define LEVBTN_MASK_MIDDLE   0x04

/* consumer side (vfs / syscall layer) */
bool evdev_mouse_present(void);
bool evdev_has_event(int idx);
int  evdev_read(int idx, void *buf, size_t len);        /* blocking, bytes  */
long evdev_ioctl(int idx, uint64_t req, uint64_t arg);  /* 0 / -errno       */
