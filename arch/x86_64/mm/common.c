#include "drivers/serial/serial.h"
#include "drivers/memory/common.h"

size_t str_len(const char *msg) {
    size_t len = 0;
    while (*msg++) len++;
    return len;
}

char *str_rev(char *str, int n) {
    for (int i = 0; i < (n - 1) / 2; i++) {
        char tmp = str[i];
        str[i] = str[n - 1 - i];
        str[n - 1 - i] = tmp;
    }
    return str;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char       *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *ptr, int c, size_t n) {
    unsigned char *p = ptr;
    while (n--) *p++ = (unsigned char)c;
    return ptr;
}

static void print_int(int num) {
    if (num == 0) { serial_print_string("0"); return; }
    char buf[12];
    int  c = 0;
    if (num < 0) { serial_print_byte('-'); num = -num; }
    unsigned int u = (unsigned int)num;
    while (u) { buf[c++] = (char)('0' + u % 10); u /= 10; }
    str_rev(buf, c);
    buf[c] = '\0';
    serial_print_string(buf);
}

static void print_hex(unsigned int num) {
    if (num == 0) { serial_print_string("0"); return; }
    char buf[10];
    int  c = 0;
    while (num) {
        int nibble = (int)(num & 0xf);
        buf[c++] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        num >>= 4;
    }
    str_rev(buf, c);
    buf[c] = '\0';
    serial_print_string("0x");
    serial_print_string(buf);
}

static void print_bin(unsigned int num) {
    if (num == 0) { serial_print_string("0"); return; }
    char buf[34];
    int  c = 0;
    while (num) { buf[c++] = (char)('0' + (num & 1)); num >>= 1; }
    str_rev(buf, c);
    buf[c] = '\0';
    serial_print_string("0b");
    serial_print_string(buf);
}

void kernel_printf(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    while (*msg) {
        if (msg[0] != '%') { serial_print_byte(*msg++); continue; }
        msg++;
        switch (*msg) {
        case 'c': serial_print_byte((char)va_arg(args, int));  break;
        case 'd': print_int(va_arg(args, int));                 break;
        case 'u': print_hex(va_arg(args, unsigned int));        break;
        case 'p': /* fall-through */
        case 'x': print_hex(va_arg(args, unsigned int));        break;
        case 'b': print_bin(va_arg(args, unsigned int));        break;
        case 's': serial_print_string(va_arg(args, char *));    break;
        default:  serial_print_byte('%'); serial_print_byte(*msg); break;
        }
        msg++;
    }
    va_end(args);
}
