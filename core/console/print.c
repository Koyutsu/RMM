#include "console/print.h"
#include "drivers/serial/serial.h"
#include <stdarg.h>

static const size_t NUM_COLS = 80;
static const size_t NUM_ROWS = 25;

struct Char {
    uint8_t character;
    uint8_t color;
};

static struct Char *const vga_buffer = (struct Char *)0xb8000;
static size_t  col   = 0;
static size_t  row   = 0;
static uint8_t color = PRINT_COLOR_WHITE | (PRINT_COLOR_BLACK << 4);

void set_col(size_t val) { col = val; }
void set_row(size_t val) { row = val; }

static void clear_row(size_t r) {
    struct Char empty = { .character = ' ', .color = color };
    for (size_t c = 0; c < NUM_COLS; c++)
        vga_buffer[c + NUM_COLS * r] = empty;
}

void print_clear(void) {
    for (size_t i = 0; i < NUM_ROWS; i++)
        clear_row(i);
    col = 0;
    row = 0;
}

void print_newline(void) {
    col = 0;
    if (row < NUM_ROWS - 1) {
        row++;
        return;
    }
    /* Scroll up one line */
    for (size_t r = 1; r < NUM_ROWS; r++)
        for (size_t c = 0; c < NUM_COLS; c++)
            vga_buffer[c + NUM_COLS * (r - 1)] = vga_buffer[c + NUM_COLS * r];
    clear_row(NUM_ROWS - 1);
}

void print_char(char character) {
    if (character == '\n') {
        print_newline();
        return;
    }
    /* headless mirror: early boot log also reaches COM1 so boot tests
     * capture the full sequence (term.c mirrors the ANSI stream later) */
    if (character != '\r') serial_print_byte(character);
    if (col >= NUM_COLS)
        print_newline();
    vga_buffer[col + NUM_COLS * row] = (struct Char){
        .character = (uint8_t)character,
        .color     = color,
    };
    col++;
}

void print_str(const char *str) {
    for (size_t i = 0; str[i] != '\0'; i++)
        print_char(str[i]);
}

void print_set_color(uint8_t foreground, uint8_t background) {
    color = foreground | (uint8_t)(background << 4);
}

void print_erase_to_eol(void) {
    for (size_t c = col; c < NUM_COLS; c++)
        vga_buffer[c + NUM_COLS * row] = (struct Char){
            .character = ' ', .color = color,
        };
}

/* Number printers */
static void print_uint(unsigned long v, int base) {
    char tmp[32]; int i = 0;
    if (!v) { print_char('0'); return; }
    while (v) { int d=(int)(v%(unsigned)base); tmp[i++]=(char)(d<10?'0'+d:'a'+d-10); v/=(unsigned)base; }
    while (i--) print_char(tmp[i]);   /* RMM fix: was tmp[i+1] - dropped the
                                         leading digit + printed garbage */
}

static char *itoa(int num, char *str, int base) {
    int i = 0;
    if (num == 0) { str[i++] = '0'; str[i] = '\0'; return str; }
    int neg = (num < 0 && base == 10);
    if (neg) num = -num;
    unsigned int unum = (unsigned int)num;
    while (unum) { int r=(int)(unum%(unsigned)base); str[i++]=(char)(r>9?'a'+r-10:'0'+r); unum/=(unsigned)base; }
    if (neg) str[i++] = '-';
    str[i] = '\0';
    for (int j=0; j<i/2; j++) { char t=str[j]; str[j]=str[i-j-1]; str[i-j-1]=t; }
    return str;
}

/* printf */
void printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { print_char(*p); continue; }
        p++;
        switch (*p) {
        case 'd': { char buf[16]; itoa(va_arg(args, int), buf, 10); print_str(buf); break; }
        case 'u': print_uint((unsigned long)va_arg(args, unsigned), 10); break;
        case 'x': { char buf[16]; itoa((int)va_arg(args, unsigned), buf, 16); print_str(buf); break; }
        case 'p': { print_str("0x"); char buf[16]; itoa((int)(uintptr_t)va_arg(args, void*), buf, 16); print_str(buf); break; }
        case 's': { print_str(va_arg(args, char *)); break; }
        case 'c': { print_char((char)va_arg(args, int)); break; }
        case '%': { print_char('%'); break; }
        default:  { print_char('%'); print_char(*p); break; }
        }
    }
    va_end(args);
}

/* Early-boot status lines (VGA text only) */
void writeOK(const char *msg) {
    print_newline();
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("[");
    print_set_color(PRINT_COLOR_GREEN, PRINT_COLOR_BLACK); print_str("  OK  ");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("] ");
    print_str(msg);
}
void writeInfo(const char *msg) {
    print_newline();
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("[");
    print_set_color(PRINT_COLOR_CYAN,  PRINT_COLOR_BLACK); print_str(" INFO ");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("] ");
    print_str(msg);
}
void writeWarn(const char *msg) {
    print_newline();
    print_set_color(PRINT_COLOR_WHITE,  PRINT_COLOR_BLACK); print_str("[");
    print_set_color(PRINT_COLOR_YELLOW, PRINT_COLOR_BLACK); print_str(" WARN ");
    print_set_color(PRINT_COLOR_WHITE,  PRINT_COLOR_BLACK); print_str("] ");
    print_str(msg);
}
void writeError(const char *msg) {
    print_newline();
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("[");
    print_set_color(PRINT_COLOR_RED,   PRINT_COLOR_BLACK); print_str(" ERR  ");
    print_set_color(PRINT_COLOR_WHITE, PRINT_COLOR_BLACK); print_str("] ");
    print_str(msg);
}
