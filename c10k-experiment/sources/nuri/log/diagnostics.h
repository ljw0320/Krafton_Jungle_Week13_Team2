#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include <stddef.h>

/* Best-effort stderr records. Callers bound records to 1024 bytes and ignore SIGPIPE.
 * Preserve errno and inherited descriptor flags; do not queue or retry backpressure. */
void diagnostic_write(const char *record, size_t length);
void diagnostic_error(const char *operation, int error);

#endif
