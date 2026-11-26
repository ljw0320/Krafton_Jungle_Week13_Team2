#ifndef LIVE_WIRE_H
#define LIVE_WIRE_H

#include <stddef.h>

#define LIVE_TEXT_MAX 4096

/* Internal loopback format: a two-byte network-order size followed by UTF-8. */
struct live_record {
    unsigned char bytes[2 + LIVE_TEXT_MAX];
    size_t used;
};

int live_port(const char *value);
/* 1 complete record, 0 incomplete, -1 disconnected or invalid size. */
int live_read(int fd, struct live_record *record);
/* Advances the offset after partial writes. Same return convention as live_read. */
int live_write(int fd, const struct live_record *record, size_t *offset);

#endif
