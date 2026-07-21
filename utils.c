#include "utils.h"

void int_to_ascii(int n, char str[]) {
    int i = 0;
    int is_negative = 0;

    if (n < 0) {
        is_negative = 1;
        n = -n;
    }

    do {
        str[i++] = n % 10 + '0';
        n /= 10;
    } while (n > 0);

    if (is_negative) str[i++] = '-';
    str[i] = '\0';

    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

/* memset/memcpy are on the hot path everywhere in the kernel (zeroing every
 * demand-paged/COW frame, every ATA sector, every filesystem read/write), so
 * a plain byte-at-a-time loop is a real cost. Both fill/copy 4 bytes at a
 * time once the pointer(s) involved are word-aligned, falling back to a byte
 * loop for the unaligned head/tail (and, for memcpy, for the whole span if
 * src and dest turn out to have different alignment) -- same behavior as
 * before, just fewer loop iterations on the common (aligned) case. */
void *memset(void *dest, int val, size_t len) {
    uint8_t *ptr = (uint8_t *)dest;
    uint8_t v8 = (uint8_t)val;

    while (len > 0 && ((uintptr_t)ptr & 3U) != 0) {
        *ptr++ = v8;
        len--;
    }

    if (len >= 4) {
        uint32_t v32 = (uint32_t)v8 * 0x01010101U;
        uint32_t *p32 = (uint32_t *)ptr;
        size_t words = len / 4;

        len -= words * 4;
        while (words-- > 0) *p32++ = v32;
        ptr = (uint8_t *)p32;
    }

    while (len-- > 0) *ptr++ = v8;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    while (len > 0 && ((uintptr_t)d & 3U) != 0) {
        *d++ = *s++;
        len--;
    }

    if (len >= 4 && ((uintptr_t)s & 3U) == 0) {
        uint32_t *d32 = (uint32_t *)d;
        const uint32_t *s32 = (const uint32_t *)s;
        size_t words = len / 4;

        len -= words * 4;
        while (words-- > 0) *d32++ = *s32++;
        d = (uint8_t *)d32;
        s = (const uint8_t *)s32;
    }

    while (len-- > 0) *d++ = *s++;
    return dest;
}

size_t strlen(const char *str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}
