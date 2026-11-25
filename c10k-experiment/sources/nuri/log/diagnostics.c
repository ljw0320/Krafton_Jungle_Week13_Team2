#define _POSIX_C_SOURCE 200809L
#include "diagnostics.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

void diagnostic_write(const char *record, size_t length)
{
    int previous = errno;
    struct stat info;
    if (fstat(STDERR_FILENO, &info) < 0)
        goto done;
    int output = STDERR_FILENO;
    if (S_ISFIFO(info.st_mode) || S_ISCHR(info.st_mode)) {
        /* dup shares O_NONBLOCK with the launcher; reopening gives independent flags. */
        output = open("/proc/self/fd/2", O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
        if (output < 0)
            goto done;
    }
    ssize_t result;
    do {
        result = S_ISSOCK(info.st_mode) ?
            send(output, record, length, MSG_DONTWAIT | MSG_NOSIGNAL) :
            write(output, record, length);
    } while (result < 0 && errno == EINTR);
    if (output != STDERR_FILENO)
        close(output);
done:
    errno = previous;
}

void diagnostic_error(const char *operation, int error)
{
    int previous = errno;
    char record[512];
    int size = snprintf(record, sizeof(record), "%s: %s\n", operation, strerror(error));
    if (size > 0 && (size_t)size < sizeof(record))
        diagnostic_write(record, (size_t)size);
    errno = previous;
}
