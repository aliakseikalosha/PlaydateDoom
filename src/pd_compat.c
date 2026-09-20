// Playdate-backed implementations of the stdio subset Doom uses.
// Reads look in the game's Data folder, then the pdx bundle (where the WAD
// lives); writes (savegames) go to the Data folder.
#include "pd_compat.h"

#undef fopen
#undef fclose
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef fflush
#undef fprintf
#undef vfprintf
#undef printf
#undef puts
#undef exit
#undef remove
#undef rename
#undef putchar

#include "pd_api.h"
#include "pd_glue.h"

typedef struct
{
    SDFile *file;
} pd_file_t;

static char last_message[256];

static int ends_with(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

FILE *pd_fopen(const char *path, const char *mode)
{
    FileOptions opts;
    SDFile *sd;
    pd_file_t *f;

    // Doom's .cfg files are text and read with fscanf; skip them and run on
    // built-in defaults.
    if (ends_with(path, ".cfg"))
    {
        errno = ENOENT;
        return NULL;
    }

    if (mode[0] == 'w')
        opts = kFileWrite;
    else if (mode[0] == 'a')
        opts = kFileAppend;
    else
        opts = kFileRead | kFileReadData;

    sd = pd_glue_api()->file->open(path, opts);
    if (sd == NULL)
    {
        errno = ENOENT;
        return NULL;
    }

    f = malloc(sizeof(*f));
    f->file = sd;
    return (FILE *) f;
}

int pd_fclose(FILE *stream)
{
    pd_file_t *f = (pd_file_t *) stream;
    int r = pd_glue_api()->file->close(f->file);
    free(f);
    return r;
}

size_t pd_fread(void *buf, size_t size, size_t n, FILE *stream)
{
    pd_file_t *f = (pd_file_t *) stream;
    int r;

    if (size == 0 || n == 0)
        return 0;
    r = pd_glue_api()->file->read(f->file, buf, (unsigned int) (size * n));
    return r < 0 ? 0 : (size_t) r / size;
}

size_t pd_fwrite(const void *buf, size_t size, size_t n, FILE *stream)
{
    pd_file_t *f = (pd_file_t *) stream;
    int r;

    if (size == 0 || n == 0)
        return 0;
    r = pd_glue_api()->file->write(f->file, buf, (unsigned int) (size * n));
    return r < 0 ? 0 : (size_t) r / size;
}

int pd_fseek(FILE *stream, long offset, int whence)
{
    pd_file_t *f = (pd_file_t *) stream;
    return pd_glue_api()->file->seek(f->file, (int) offset, whence);
}

long pd_ftell(FILE *stream)
{
    pd_file_t *f = (pd_file_t *) stream;
    return pd_glue_api()->file->tell(f->file);
}

int pd_fflush(FILE *stream)
{
    pd_file_t *f = (pd_file_t *) stream;

    if (stream == stdout || stream == stderr || stream == NULL)
        return 0;
    return pd_glue_api()->file->flush(f->file);
}

static void log_line(const char *s)
{
    size_t n = strlen(s);

    // Keep the most recent message; I_Error prints the reason just before
    // exiting, and pd_exit shows it on screen.
    strncpy(last_message, s, sizeof(last_message) - 1);
    last_message[sizeof(last_message) - 1] = '\0';
    while (n > 0 && (last_message[n - 1] == '\n' || last_message[n - 1] == ' '))
        last_message[--n] = '\0';
    if (n > 0)
        pd_glue_api()->system->logToConsole("%s", last_message);
}

int pd_vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (stream == stdout || stream == stderr)
    {
        if (stream == stderr || buf[0] != '\0')
            log_line(buf);
        return n;
    }
    if (n > (int) sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    return pd_fwrite(buf, 1, n, stream) == (size_t) n ? n : -1;
}

int pd_fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = pd_vfprintf(stream, fmt, ap);
    va_end(ap);
    return n;
}

int pd_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = pd_vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int pd_puts(const char *s)
{
    log_line(s);
    return 0;
}

void pd_exit(int code)
{
    // system->error shows the message on the device / simulator and halts.
    pd_glue_api()->system->error("Doom exited (%d): %s", code, last_message);
    for (;;)
    {
    }
}

int pd_remove(const char *path)
{
    return pd_glue_api()->file->unlink(path, 0);
}

int pd_rename(const char *from, const char *to)
{
    return pd_glue_api()->file->rename(from, to);
}

#if TARGET_PLAYDATE
// newlib's snprintf/sscanf link its stdio internals, which reference these
// syscalls. Nothing here uses real file descriptors, so they just fail.
#include <sys/stat.h>

int _write(int fd, const void *buf, size_t n) { (void) fd; (void) buf; (void) n; return -1; }
int _read(int fd, void *buf, size_t n) { (void) fd; (void) buf; (void) n; return -1; }
int _close(int fd) { (void) fd; return -1; }
long _lseek(int fd, long off, int whence) { (void) fd; (void) off; (void) whence; return -1; }
int _fstat(int fd, struct stat *st) { (void) fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void) fd; return 1; }
int _unlink(const char *p) { (void) p; return -1; }
int _link(const char *a, const char *b) { (void) a; (void) b; return -1; }
#endif
