// Mimas -- a Doom engine for the Sega Saturn.
// Copyright (C) 2025-2026 Romain Cicolini (N0rt0N85).
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 2 of the License, or (at your option)
// any later version.  Distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (the COPYING file at the repo root, or
// <https://www.gnu.org/licenses/>) for details.
//
/*
** Mimas -- newlib syscall stubs.
** stdout/stderr go to the on-screen debug console; there is no real
** filesystem (config/savegames silently fail); sbrk serves a static
** heap in high work RAM.
**
** Unchanged from SaturnDoom.  The Ymir emulator debug port write
** (0x22100001) is preserved so that printf output appears in Ymir's
** host-side console even when using SRL.
*/
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#undef errno
extern int errno;

void sat_console_putc(char c);
void DG_Fatal(const char *msg);

/* Ymir/Mednafen emulator debug port: intercepted by Ymir, ignored on
   Kronos and real hardware. */
static void emu_debug_putc(char c)
{
    *(volatile unsigned char *)0x22100001 = (unsigned char)c;
}

char *__env[1] = { 0 };
char **environ = __env;

/* SATURN: the newlib libc heap is a dedicated static array, kept SEPARATE from
   SRL's TLSF pool (which owns the whole linker __heap_start..__heap_end region --
   see srl_memory.hpp).  Every KB of this .bss array costs the TLSF pool above _end
   1 KB, so it is sized as tight as is safe.

   HISTORY: its dominant consumer USED to be W_AddFile's lumpinfo array (numlumps *
   28 B; Doom II = 81.7 KB), which forced an 88 KB heap and starved the TLSF pool to
   ~4 KB -- too tight (the 3p minimap's code then boot-looped the pool).  FIX: lumpinfo
   was MOVED to the roomy ~1MB LWRAM Doom zone (ExtendLumpInfo in core/w_wad.c, the move
   anticipated by the old note here), so the libc heap no longer scales with the IWAD and
   ALL IWADs are still supported.  What remains here is incidental (printf/stdio buffers,
   ~1-2 KB MEASURED peak, see below), so the heap is trimmed 88 -> 32 KB, returning ~56 KB to the TLSF pool.
   Watch row-10 `hp` (dg_heap_peak) stays < HEAP_SIZE on a full E1 run; trim further toward
   the measured peak if you want even more pool, or raise back if a hidden libc alloc appears. */
#define HEAP_SIZE (4 * 1024)    /* SATURN 2026-08-12 (macro plan P0): 12 -> 4 KB = +8192 B of TLSF
                                    pool, EXACTLY.  This is the one pool change whose gain is
                                    arithmetic instead of section-layout luck: __heap_end is a
                                    link-time constant (0x060fa000 in all ten build/*.map) and the
                                    .bss downstream of syscalls.o aligns to at most 0x10, so a
                                    multiple-of-16 cut lands 1:1.  Contrast the usual trap -- deleting
                                    CODE kept moving the pool DOWN (4.98->4.80, 4.73->4.11, and again
                                    8.41->7.97 on 2026-08-09) because `_end` follows section layout,
                                    not byte count.
                                      TAKEN ON A MEASUREMENT, not an estimate.  The overlay `hp` field
                                    was restored on 2026-08-09 and immediately read hp1/12k across
                                    three TNT MAP11 captures at level time t4s-t20s, boot included:
                                    the peak is in [1024, 2047] B (the old field truncated >>10).  The
                                    ~6 KB figure asserted here for a year was never checked once the
                                    instrument existed.  4096 B is ~2x the measured ceiling and ~2.5x
                                    the ENUMERATED one (1028 B stdout BUFSIZ + 416 B FILE glue + ~204 B
                                    of M_StringJoin/strdup/I_AtExit = ~1648 B).
                                      THE RISK IS THE **LOAD** GATE, NOT PERF: an sbrk failure halts in
                                    M_StringJoin / M_StringDuplicate (core/m_misc.c) BEFORE the first
                                    frame.  So `hp` now prints BYTES, and _sbrk's ENOMEM branch -- mute
                                    until today -- increments dg_heap_fail, shown as `hp<peak>/<cap>!<n>`.
                                    ANY `!` on row 10 means raise HEAP_SIZE, and it is the ONLY warning
                                    you get.  The old note here claimed W_AddFile's lumpinfo calloc was
                                    the thing to fear: it is NOT -- that array is a Z_Malloc in the
                                    LWRAM zone (core/w_wad.c:124-129) and `calloc` is referenced by no
                                    project object at all (build/Mimas-Tnt.map pulls it only via
                                    libc_a-mprec.o).  The late allocator to watch instead is the save
                                    menu's fopen (core/m_menu.c:513).
                                      History: 88->32->24->20->18->16->12->4 KB.
                                    WALL_ACC_MAX stays 128 -- never rob the wall budget for the pool. */
static char heap[HEAP_SIZE] __attribute__((aligned(8)));
static char *heap_end = heap;

/* SATURN VALIDATION (#4 newlib-heap trim): high-water mark of the static libc
   sbrk heap (NOT the Doom zone, which lives in LWRAM).  Almost everything in Doom
   goes through Z_Malloc; this heap only serves incidental libc allocs (printf
   buffers, W_AddFile's lumpinfo calloc).  Read dg_heap_peak on Ymir across a full
   E1 + WAD load to size HEAP_SIZE down (recovers HWRAM .bss).  Ymir is HONEST here:
   the high-water is allocation-count driven, not bus/timing.  Exposed for the
   overlay; do NOT trim HEAP_SIZE until the peak is measured (W_AddFile calloc
   failing would brick the WAD load -- failure is graceful NULL, not corruption). */
int dg_heap_peak = 0;            /* bytes ever sbrk'd (high-water)        */
int dg_heap_size = HEAP_SIZE;    /* the cap, for the overlay denominator  */
int dg_heap_fail = 0;            /* sbrk refusals -- see the ENOMEM branch */

void *_sbrk(int incr)
{
    char *prev = heap_end;
    int  used;
    if (heap_end + incr > heap + HEAP_SIZE)
    {
        /* SATURN 2026-08-12: this branch was MUTE.  With HEAP_SIZE cut to 2x the measured peak it is
           the only thing standing between a hidden libc alloc and a LOAD-gate halt with no cause
           attached, so it is now counted and surfaced as row-10 `hp<peak>/<cap>!<n>`.  It does NOT
           make the failure survivable -- malloc returns NULL and the caller usually I_Errors -- it
           makes it ATTRIBUTABLE, which is the difference between "raise HEAP_SIZE by 4 KB" and a
           week of bisecting a boot loop. */
        dg_heap_fail++;
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    used = (int)(heap_end - heap);
    if (used > dg_heap_peak)
        dg_heap_peak = used;
    return prev;
}

int _write(int fd, const char *buf, int len)
{
    int i;
    (void)fd;
    for (i = 0; i < len; ++i) {
        sat_console_putc(buf[i]);
        emu_debug_putc(buf[i]);
    }
    return len;
}

int _read(int fd, char *buf, int len)   { (void)fd; (void)buf; (void)len; return 0; }

int _open(const char *name, int flags, int mode)
{
    (void)name; (void)flags; (void)mode;
    errno = ENOENT;
    return -1;
}

int _close(int fd)                      { (void)fd; return -1; }
int _lseek(int fd, int ptr, int dir)    { (void)fd; (void)ptr; (void)dir; return 0; }

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    (void)path; (void)st;
    errno = ENOENT;
    return -1;
}

int _isatty(int fd)                     { (void)fd; return 1; }

int _kill(int pid, int sig)
{
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void)                       { return 1; }

int _unlink(const char *name)
{
    (void)name;
    errno = ENOENT;
    return -1;
}

int _link(const char *old, const char *newp)
{
    (void)old; (void)newp;
    errno = EMLINK;
    return -1;
}

int _gettimeofday(void *tv, void *tz)   { (void)tv; (void)tz; return -1; }

void _exit(int status)
{
    (void)status;
    DG_Fatal("exit() called");
    for (;;) ;
}

void exit(int status)   { _exit(status); }
void abort(void)        { _exit(1); }

int rename(const char *old, const char *newp)
{
    (void)old; (void)newp;
    errno = ENOENT;
    return -1;
}

int remove(const char *path)            { (void)path; errno = ENOENT; return -1; }
int system(const char *cmd)             { (void)cmd; return -1; }

int mkdir(const char *path, mode_t mode)
{
    (void)path; (void)mode;
    errno = EACCES;
    return -1;
}

double fabs(double x)                   { return x < 0.0 ? -x : x; }
