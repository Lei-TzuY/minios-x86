#ifndef IO_H
#define IO_H

#include <stdint.h>

/*
 * Port I/O. These in/out instructions are privileged and fault in ring 3, so
 * under HOSTED_TEST (the native unit tests) they compile to harmless no-ops --
 * a test that reaches real hardware access is not meaningful on the host, and
 * this lets modules that only touch ports incidentally (e.g. timer_install)
 * link and run there. The kernel build never defines HOSTED_TEST. */
static inline void outb(uint16_t port, uint8_t val) {
#ifndef HOSTED_TEST
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
#else
    (void)port; (void)val;
#endif
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret = 0;
#ifndef HOSTED_TEST
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
#else
    (void)port;
#endif
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret = 0;
#ifndef HOSTED_TEST
    __asm__ volatile ( "inw %1, %0" : "=a"(ret) : "Nd"(port) );
#else
    (void)port;
#endif
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
#ifndef HOSTED_TEST
    __asm__ volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) );
#else
    (void)port; (void)val;
#endif
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif
