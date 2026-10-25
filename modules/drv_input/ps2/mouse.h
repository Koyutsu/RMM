#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * mouse - PS/2 auxiliary-port driver
 *
 * Enables the i8042 aux port (IRQ12), decodes the standard 3-byte
 * stream-mode packets and feeds them into the evdev layer as
 * REL_X/REL_Y + BTN_LEFT/RIGHT/MIDDLE batches on /dev/input/event1.
 *
 * All controller waits are bounded - a machine without a PS/2 mouse
 * (or with USB legacy emulation hiding it) fails init cleanly and the
 * system boots exactly as before, minus event1.
 */

bool mouse_init(void);      /* call once from kernel_main, after kb_init */
bool mouse_enabled(void);   /* true once init succeeded (packets flowing) */
