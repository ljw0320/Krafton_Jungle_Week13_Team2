#include "server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

static int parse_port(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "usage: %s PORT DOCUMENT_ROOT [CGI_ROOT]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *end;
    errno = 0;
    long value = strtol(argv[1], &end, 10);

    if (errno || end == argv[1] || *end || value < 1 || value > 65535) {
        fprintf(stderr, "port must be an integer from 1 to 65535\n");
        exit(EXIT_FAILURE);
    }

    return (int)value;
}

static void server_setup(struct server *server, const char *root, const char *cgi_root)
{
    server->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (server->root_fd == -1)
        fail("document root");

    server->cgi_root_fd = cgi_root ? open(cgi_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    if (cgi_root && server->cgi_root_fd == -1)
        fail("CGI root");

    server->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (server->wake_fd == -1)
        fail("eventfd");

    check_thread(pthread_mutex_init(&server->mutex, NULL));
    check_thread(pthread_cond_init(&server->ready, NULL));

    signals_start(server->wake_fd);
    pool_start(server);
    signals_unblock();
}

static void server_cleanup(struct server *server, int listener)
{
    close(listener);
    pool_stop(server);

    for (int i = 0; i < MAX_CONNECTIONS; i++)
        if (server->clients[i].state != FREE)
            connection_close(&server->clients[i]);

    signals_detach();
    close(server->wake_fd);
    close(server->root_fd);
    if (server->cgi_root_fd >= 0)
        close(server->cgi_root_fd);

    check_thread(pthread_cond_destroy(&server->ready));
    check_thread(pthread_mutex_destroy(&server->mutex));

    puts("[server] stopped");
}

/* 초기화 → 준비된 연결 배정 → 종료 신호 후 join·자원 회수 순서입니다. */
int main(int argc, char **argv)
{
    static struct server server;
    int port = parse_port(argc, argv);
    if (setvbuf(stdout, NULL, _IOLBF, 0))
        fail("setvbuf");

    int listener = listener_open(port);
    server_setup(&server, argv[2], argc == 4 ? argv[3] : NULL);

    printf("[server] http://127.0.0.1:%d root=%s workers=%d slots=%d\n",
           port, argv[2], WORKERS, MAX_CONNECTIONS);

    server_run(&server, listener);
    server_cleanup(&server, listener);

    return EXIT_SUCCESS;
}
