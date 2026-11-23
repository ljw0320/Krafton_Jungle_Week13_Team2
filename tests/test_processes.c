#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/wait.h>

static int wait_step;
static int wait_error;
static int kill_error;
static int kill_calls;
static pid_t expected_pids[2];
static int expected_kills;

static pid_t fake_waitpid(pid_t pid, int *status, int options)
{
    assert(pid == -1 && status == NULL);
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
    assert(kill_calls < expected_kills);
    assert(pid == expected_pids[kill_calls] && signo == SIGTERM);
    ++kill_calls;
    errno = kill_error;
    return kill_error ? -1 : 0;
}

#define waitpid fake_waitpid
#define kill fake_kill
#include "../server/server.c"

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
    expected_pids[0] = 202;
    expected_kills = 1;
    assert(signal_children(&children, SIGTERM) == 0);
    assert(kill_calls == 1);
    kill_calls = 0;
    kill_error = ESRCH;
    assert(signal_children(&children, SIGTERM) == 0);
    assert(kill_calls == 1);
    pids[0] = 101;
    pids[1] = 202;
    children.active = 2;
    expected_pids[0] = 101;
    expected_pids[1] = 202;
    expected_kills = 2;
    kill_calls = 0;
    kill_error = EPERM;
    assert(signal_children(&children, SIGTERM) == -1);
    assert(kill_calls == 2);
    puts("PASS: process error handling (expected diagnostics above)");
    return 0;
}
