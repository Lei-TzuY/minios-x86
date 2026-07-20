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

void *memset(void *dest, int val, size_t len) {
    uint8_t *ptr = (uint8_t *)dest;
    while (len-- > 0)
        *ptr++ = (uint8_t)val;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (len-- > 0)
        *d++ = *s++;
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
