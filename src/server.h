#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>

#define SERVER_RCVBUF 16384

int server_listen(uint32_t addr, uint16_t port);

#endif
