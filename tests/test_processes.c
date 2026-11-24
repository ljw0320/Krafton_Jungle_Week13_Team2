#define _GNU_SOURCE
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
#undef waitpid
#undef kill
#include "test_runner.h"

static void reset_mocks(void)
{
    wait_step = 0;
    wait_error = 0;
    kill_error = 0;
    kill_calls = 0;
    expected_kills = 0;
}

static void test_reap_retries_interruption_and_removes_exited_pid(void)
{
    reset_mocks();
    pid_t pids[] = {101, 202};
    struct child_processes children = {.pids = pids, .capacity = 2, .active = 2};
    assert(reap_children(&children) == 0);
    assert(children.active == 1 && children.pids[0] == 202);
}

static void test_reap_accepts_no_children(void)
{
    reset_mocks();
    pid_t pids[] = {202};
    struct child_processes children = {.pids = pids, .capacity = 1, .active = 1};
    wait_error = ECHILD;
    assert(reap_children(&children) == 0);
    assert(children.active == 1);
}

static void test_reap_reports_unexpected_error(void)
{
    reset_mocks();
    pid_t pids[] = {202};
    struct child_processes children = {.pids = pids, .capacity = 1, .active = 1};
    wait_error = EINVAL;
    assert(reap_children(&children) == -1);
    assert(children.active == 1);
}

static void test_signal_targets_active_child(void)
{
    reset_mocks();
    pid_t pids[] = {202};
    struct child_processes children = {.pids = pids, .capacity = 1, .active = 1};
    expected_pids[0] = 202;
    expected_kills = 1;
    assert(signal_children(&children, SIGTERM) == 0);
    assert(kill_calls == 1);
}

static void test_signal_accepts_already_exited_child(void)
{
    reset_mocks();
    pid_t pids[] = {202};
    struct child_processes children = {.pids = pids, .capacity = 1, .active = 1};
    expected_pids[0] = 202;
    expected_kills = 1;
    kill_error = ESRCH;
    assert(signal_children(&children, SIGTERM) == 0);
    assert(kill_calls == 1);
}

static void test_signal_continues_after_permission_error(void)
{
    reset_mocks();
    pid_t pids[] = {101, 202};
    struct child_processes children = {.pids = pids, .capacity = 2, .active = 2};
    expected_pids[0] = 101;
    expected_pids[1] = 202;
    expected_kills = 2;
    kill_calls = 0;
    kill_error = EPERM;
    assert(signal_children(&children, SIGTERM) == -1);
    assert(kill_calls == 2);
}

int main(int argc, char **argv)
{
    const struct test_case tests[] = {
        TEST_CASE(test_reap_retries_interruption_and_removes_exited_pid),
        TEST_CASE(test_reap_accepts_no_children),
        TEST_CASE(test_reap_reports_unexpected_error),
        TEST_CASE(test_signal_targets_active_child),
        TEST_CASE(test_signal_accepts_already_exited_child),
        TEST_CASE(test_signal_continues_after_permission_error),
    };
    return run_tests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
