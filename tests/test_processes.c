#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

static int wait_step;
static int wait_error;
static int kill_error;
static int kill_calls;

static pid_t fake_waitpid(pid_t pid, int *status, int options)
{
    (void)pid;
    (void)status;
    assert(options == WNOHANG);
    if (wait_error) {
        errno = wait_error;
        return -1;
    }
    if (wait_step++ == 0) {
        errno = EINTR;
        return -1;
    }
    return wait_step == 2 ? 101 : 0;
}

static int fake_kill(pid_t pid, int signo)
{
    assert(pid > 0 && signo == SIGTERM);
    ++kill_calls;
    errno = kill_error;
    return kill_error ? -1 : 0;
}

#define waitpid fake_waitpid
#define kill fake_kill
#define main server_main
#include "../server.c"
#undef main

int main(void)
{
    pid_t pids[] = {101, 202};
    struct child_processes children = {.pids = pids, .capacity = 2, .active = 2};
    assert(reap_children(&children) == 0);
    assert(children.active == 1 && children.pids[0] == 202);
    wait_error = ECHILD;
    assert(reap_children(&children) == 0);
    wait_error = EINVAL;
    assert(reap_children(&children) == -1);
    assert(children.active == 1);
    assert(signal_children(&children, SIGTERM) == 0);
    kill_error = ESRCH;
    assert(signal_children(&children, SIGTERM) == 0);
    children.active = 2;
    kill_calls = 0;
    kill_error = EPERM;
    assert(signal_children(&children, SIGTERM) == -1);
    assert(kill_calls == 2);
    puts("PASS: process error handling (expected diagnostics above)");
    return 0;
}
