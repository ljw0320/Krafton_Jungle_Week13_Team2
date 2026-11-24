#ifndef SERVER_H
#define SERVER_H

/* Runs once per process with validated positive CLI values and a root path.
 * Installs process-wide signal handlers and masks; releases server resources.
 * Returns EXIT_SUCCESS or EXIT_FAILURE. */
int server_run(int port, const char *root, int max_children,
               int idle_seconds, int grace_seconds, int backend_limit);

#endif
