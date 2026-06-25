/*
** saturn_libc/stdlib.h -- full stdlib declarations for Mimas.
**
** SGL's modules/sgl/INC/stdlib.h is a minimal stub (no malloc/free/strdup).
** This header shadows it by being found first via -Isaturn_libc.
** Implementations come from the linked sh2eb-elf newlib (sh-elf/lib/libc.a).
*/

#ifndef _SATURN_STDLIB_H_
#define _SATURN_STDLIB_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- Memory allocation ------------------------------------------------- */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);

/* -- String conversion ------------------------------------------------- */
int           atoi(const char *nptr);
long          atol(const char *nptr);
long long     atoll(const char *nptr);
double        atof(const char *nptr);
long          strtol(const char *nptr, char **endptr, int base);
long long     strtoll(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
double        strtod(const char *nptr, char **endptr);

/* -- Math -------------------------------------------------------------- */
int  abs(int j);
long labs(long j);
int  rand(void);
void srand(unsigned int seed);

/* -- Algorithms -------------------------------------------------------- */
void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* -- String utilities (POSIX extensions) ------------------------------- */
char *strdup(const char *s);

/* -- Process control --------------------------------------------------- */
void exit(int status) __attribute__((noreturn));
void _exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#endif
#ifndef EXIT_FAILURE
#define EXIT_FAILURE 1
#endif
#ifndef NULL
#define NULL ((void *)0)
#endif
#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

#endif /* _SATURN_STDLIB_H_ */
