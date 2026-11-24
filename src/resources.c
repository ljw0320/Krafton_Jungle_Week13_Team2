#include "server.h"
#include <unistd.h>

bool resource_acquire(atomic_uint *count, unsigned limit)
{
    unsigned used = atomic_load(count);
    while (used < limit) {
        if (atomic_compare_exchange_weak(count, &used, used + 1))
            return true;
    }
    return false;
}

void file_release(struct connection *client)
{
    if (client->file_fd >= 0)
        close(client->file_fd);
    client->file_fd = -1;
    if (client->file_reserved) {
        atomic_fetch_sub(&client->resource_server->active_files, 1);
        client->file_reserved = false;
    }
}
