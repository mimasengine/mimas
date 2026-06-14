/*
** DoomSRL -- newlib syscall stubs.
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

#define HEAP_SIZE (64 * 1024)
static char heap[HEAP_SIZE] __attribute__((aligned(8)));
static char *heap_end = heap;

void *_sbrk(int incr)
{
    char *prev = heap_end;
    if (heap_end + incr > heap + HEAP_SIZE)
    {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
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

void __call_exitprocs(int code, void *d) { (void)code; (void)d; }
