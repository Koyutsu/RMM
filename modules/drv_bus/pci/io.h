#pragma once
#include <stdint.h>

static inline uint8_t inportb(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outportb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint16_t inportw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outportw(uint16_t port, uint16_t v) {
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline uint32_t inportl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void outportl(uint16_t port, uint32_t v) {
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port));
}
