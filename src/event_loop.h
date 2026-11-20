#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>

#define MAX_EVENTS 64

struct conn;

int loop_run(int listen_fd);
int loop_watch(struct conn *c, uint32_t events);
void loop_unwatch(struct conn *c);

#endif
