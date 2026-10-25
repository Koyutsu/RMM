#include <stddef.h>
#include <stdbool.h>
#include "drivers/serial/serial.h"
#include "drivers/serial/io.h"

#define PORT 0x3F8

static bool serial_initialized = false;

static bool init_serial(void) {
    out_byte(PORT + 1, 0x00);
    out_byte(PORT + 3, 0x80);
    out_byte(PORT + 0, 0x03);
    out_byte(PORT + 1, 0x00);
    out_byte(PORT + 3, 0x03);
    out_byte(PORT + 2, 0xC7);
    out_byte(PORT + 4, 0x0B);
    out_byte(PORT + 4, 0x1E);
    out_byte(PORT + 0, 0xAE);
    if (in_byte(PORT + 0) != 0xAE) return false;
    out_byte(PORT + 4, 0x0F);
    return true;
}

void serial_print_byte(const char c) {
    if (!serial_initialized) {
        init_serial();
        for (int i = 0; i < 50; i++) out_byte(PORT, '-');
        out_byte(PORT, '\n');
        serial_initialized = true;
    }
    /* operator precedence - must be (... & 0x20) == 0 */
    while ((in_byte(PORT + 5) & 0x20) == 0);
    out_byte(PORT, (unsigned char)c);
}

void serial_print_string(const char *msg) {
    while (*msg) serial_print_byte(*msg++);
}

bool serial_has_input(void) {
    if (!serial_initialized) return false;
    return (in_byte(PORT + 5) & 0x01) != 0;
}

int serial_getchar_nb(void) {
    if (!serial_initialized) return 0;
    /* LSR bit 0 = Data Ready */
    if ((in_byte(PORT + 5) & 0x01) == 0) return 0;
    return (unsigned char)in_byte(PORT);   /* RBR clears the ready bit */
}

