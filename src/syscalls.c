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
   ~6 KB peak), so the heap is trimmed 88 -> 32 KB, returning ~56 KB to the TLSF pool.
   Watch row-22 `hp` (dg_heap_peak) stays < HEAP_SIZE on a full E1 run; trim further toward
   the measured peak if you want even more pool, or raise back if a hidden libc alloc appears. */
#define HEAP_SIZE (20 * 1024)   /* 32->24->20KB (20KB 2026-07-15): reclaim 4KB HWRAM .bss to the TLSF pool
                                    for the M7-multi split-lowres .text (which had pushed the pool below
                                    the 4KB boot-loop floor -> 3776B; now ~8KB pool).  Peak is ~6KB
                                    (lumpinfo is in LWRAM, no big-IWAD spike) so 20KB keeps ~14KB headroom
                                    -- deliberately NOT 16KB, to stay conservative for un-measured big-WAD
                                    (TNT/Plutonia) libc transients; watch row-22 hp < 20KB on a big-WAD run.
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

void *_sbrk(int incr)
{
    char *prev = heap_end;
    int  used;
    if (heap_end + incr > heap + HEAP_SIZE)
    {
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
