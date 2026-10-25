#include "drivers/ps2/memory_op.h"

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

void zero_char(char *buffer, uint8_t length) {
    for (int i = 0; i < (int)length; i++)
        buffer[i] = 0;
}
