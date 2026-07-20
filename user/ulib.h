#ifndef ULIB_H
#define ULIB_H

/* Tiny freestanding helpers shared by miniOS user programs. */

static inline int uatoi(const char *s) {
    int value = 0, neg = 0;

    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }
    return neg ? -value : value;
}

static inline int ustrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static inline void umemcpy(void *dst, const void *src, unsigned n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (unsigned i = 0; i < n; i++) d[i] = s[i];
}

#endif
