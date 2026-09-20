// Force-included into every Doom source file (see CMakeLists.txt).
// Redirects the libc pieces Doom relies on (stdio files, console output,
// exit) to the Playdate API, which has no working stdio or process model.
#ifndef PD_COMPAT_H
#define PD_COMPAT_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *pd_fopen(const char *path, const char *mode);
int pd_fclose(FILE *f);
size_t pd_fread(void *buf, size_t size, size_t n, FILE *f);
size_t pd_fwrite(const void *buf, size_t size, size_t n, FILE *f);
int pd_fseek(FILE *f, long offset, int whence);
long pd_ftell(FILE *f);
int pd_fflush(FILE *f);
int pd_fprintf(FILE *f, const char *fmt, ...);
int pd_vfprintf(FILE *f, const char *fmt, va_list ap);
int pd_printf(const char *fmt, ...);
int pd_puts(const char *s);
int pd_remove(const char *path);
int pd_rename(const char *from, const char *to);
void pd_exit(int code) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#define fopen pd_fopen
#define fclose pd_fclose
#define fread pd_fread
#define fwrite pd_fwrite
#define fseek pd_fseek
#define ftell pd_ftell
#define fflush pd_fflush
#define fprintf pd_fprintf
#define vfprintf pd_vfprintf
#define printf pd_printf
#define puts pd_puts
#define remove pd_remove
#define rename pd_rename
#define putchar(c) (c)
#define exit pd_exit

// No shell, directories or terminals on the device.
#define system(cmd) (-1)
#define mkdir(...) (0)
#define isatty(fd) (0)

#endif
