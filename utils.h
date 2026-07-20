#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

void int_to_ascii(int n, char str[]);
void *memset(void *dest, int val, size_t len);
void *memcpy(void *dest, const void *src, size_t len);
size_t strlen(const char *str);
int strcmp(const char *s1, const char *s2);
char *strcpy(char *dest, const char *src);

#endif
