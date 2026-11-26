#include "live_wire.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/socket.h>

int live_port(const char *value) {
    char *end;
    long port = strtol(value, &end, 10);
    return *value && !*end && port >= 1 && port <= 65535 ? (int)port : -1;
}

static int retry(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

int live_read(int fd, struct live_record *record) {
    for (;;) {
        size_t target = 2;
        if (record->used >= 2) {
            size_t length = ((size_t)record->bytes[0] << 8) | record->bytes[1];
            if (!length || length > LIVE_TEXT_MAX) return -1;
            target += length;
            if (target == record->used) return 1;
        }
        ssize_t n = recv(fd, record->bytes + record->used, target - record->used, MSG_DONTWAIT);
        if (n <= 0) return n < 0 && retry() ? 0 : -1;
        record->used += (size_t)n;
    }
}

int live_write(int fd, const struct live_record *record, size_t *offset) {
    ssize_t n = send(fd, record->bytes + *offset, record->used - *offset, MSG_DONTWAIT);
    if (n <= 0) return n < 0 && retry() ? 0 : -1;
    *offset += (size_t)n;
    return *offset == record->used;
}
