/*
** saturn_libc/stdio.h -- redirect to the real newlib stdio.h.
**
** modules/dummy/stdio.h stubs printf to ((void)0) and omits FILE entirely.
** We remove that dir from CCFLAGS (see Makefile) and chain here to the real
** newlib stdio.h via #include_next.  The real newlib defines stdin/stdout/stderr
** via _REENT (reentrant struct) and exports the full FILE API backed by _write().
**
** saturn_libc/ appears first in -I so this file intercepts the include and
** routes past any remaining stubs to the actual sh2eb-elf newlib header.
*/
#pragma once
#include_next <stdio.h>
