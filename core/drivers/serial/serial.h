#pragma once
#include <stdint.h>
#include <stdbool.h>

void serial_print_byte(const char character);
void serial_print_string(const char * msg);

/* Non-blocking receive: returns the byte read, or 0 when no data is
 * pending.  Used by the console layer so COM1 doubles as a Linux-style
 * serial console input device (qemu -serial stdio, headless testing). */
int  serial_getchar_nb(void);
bool serial_has_input(void);