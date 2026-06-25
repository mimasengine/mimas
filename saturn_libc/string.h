/*
** saturn_libc/string.h -- full string declarations for Mimas.
**
** SGL's modules/sgl/INC/string.h is a minimal stub (no strdup, strtok, etc.).
** This header shadows it via -Isaturn_libc appearing first in the -I list.
** Implementations come from the linked sh2eb-elf newlib.
*/

#ifndef _SATURN_STRING_H_
#define _SATURN_STRING_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Memory functions -------------------------------------------------- */
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *s, int c, size_t n);
int    memcmp(const void *s1, const void *s2, size_t n);
void  *memchr(const void *s, int c, size_t n);

/* -- String functions -------------------------------------------------- */
size_t strlen(const char *s);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *s1, const char *s2);
int    strncmp(const char *s1, const char *s2, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delim);
char  *strerror(int errnum);

/* -- POSIX extensions -------------------------------------------------- */
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
int    strcasecmp(const char *s1, const char *s2);
int    strncasecmp(const char *s1, const char *s2, size_t n);

#ifdef __cplusplus
}
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* _SATURN_STRING_H_ */
