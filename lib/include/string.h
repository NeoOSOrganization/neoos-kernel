#ifndef NEOOS_STRING_H
#define NEOOS_STRING_H

#include <stdint.h>

uint64_t strlen(const char *s);
void *memcpy(void *dst, const void *src, uint64_t n);
void *memset(void *s, int c, uint64_t n);
void *memmove(void *dst, const void *src, uint64_t n);

// Standard C semantics: negative, zero or positive according to the
// first differing byte, compared as unsigned char.
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, uint64_t n);
int memcmp(const void *a, const void *b, uint64_t n);

#endif
