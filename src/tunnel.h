#ifndef TUNNEL_H
#define TUNNEL_H

#include <stdint.h>
#include <time.h>

#define TUNNEL_SOCKBUF 16384

struct conn;
int tunnel_start(struct conn *c);
int tunnel_on_client(struct conn *c, uint32_t events);
int tunnel_on_backend(struct conn *c, uint32_t events);
void tunnel_sweep(struct conn *c, time_t now);

#endif
