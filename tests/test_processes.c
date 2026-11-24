#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../proxy/routes.h"

static int failure_stage;
static int resource_errno;
static int poll_calls;
static int accepted_fd;
static int witness_fd;
static int control_fds[2];
static volatile sig_atomic_t *stop_flag;

static int fake_accept(int fd, struct sockaddr *address, socklen_t *length)
{
    if (!failure_stage)
        return accept(fd, address, length);
    if (failure_stage == 1) {
        errno = resource_errno;
        return -1;
    }
    return accepted_fd = dup(witness_fd);
}

static int fake_socketpair(int domain, int type, int protocol, int fds[2])
{
    if (failure_stage == 2) {
        errno = resource_errno;
        return -1;
    }
    int result = socketpair(domain, type, protocol, fds);
    if (failure_stage && result == 0) {
        control_fds[0] = fds[0];
        control_fds[1] = fds[1];
    }
    return result;
}

static pid_t fake_fork(void)
{
    if (!failure_stage)
        return fork();
    errno = resource_errno;
    return -1;
}

static bool fake_routes_enabled(void)
{
    return failure_stage || routes_enabled();
}

static int fake_ppoll(struct pollfd *fds, nfds_t count, const struct timespec *timeout,
                       const sigset_t *mask)
{
    if (!failure_stage)
        return ppoll(fds, count, timeout, mask);
    ++poll_calls;
    assert(count == 1);
    if (poll_calls == 1) {
        assert(fds[0].fd >= 0 && timeout == NULL);
        fds[0].revents = POLLIN;
        return 1;
    }
    assert(fds[0].fd == -1 && timeout != NULL);
    assert(timeout->tv_sec == 0 && timeout->tv_nsec > 0 && timeout->tv_nsec <= 100000000);
    if (poll_calls == 2) {
        errno = EINTR;
        return -1;
    }
    *stop_flag = 1;
    return 0;
}

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
#define accept fake_accept
#define socketpair fake_socketpair
#define fork fake_fork
#define routes_enabled fake_routes_enabled
#define ppoll fake_ppoll
#include "../server/server.c"
#undef waitpid
#undef kill
#undef accept
#undef socketpair
#undef fork
#undef routes_enabled
#undef ppoll
#include "test_runner.h"

static void reset_mocks(void)
{
    wait_step = 0;
    wait_error = 0;
    kill_error = 0;
    kill_calls = 0;
    expected_kills = 0;
    failure_stage = 0;
}

static void run_resource_failure(int stage, int error)
{
    reset_mocks();
    int witness[2] = {socket(AF_INET, SOCK_STREAM, 0), socket(AF_INET, SOCK_STREAM, 0)};
    assert(witness[0] >= 0 && witness[1] >= 0);
    failure_stage = stage;
    resource_errno = error;
    witness_fd = witness[0];
    accepted_fd = control_fds[0] = control_fds[1] = -1;
    poll_calls = 0;
    stopping = 0;
    stop_flag = &stopping;
    pid_t pids[1];
    struct child_channel channels[1];
    struct pollfd pollfds[2];
    struct child_processes children = {.pids = pids, .channels = channels,
        .pollfds = pollfds, .capacity = 1, .backend_limit = 1};
    sigset_t mask;
    sigemptyset(&mask);
    int result = run_parent(witness[0], -1, &children, 1, &mask);
    if (error == EINVAL)
        assert(result == EXIT_FAILURE && poll_calls == 1);
    else
        assert(result == EXIT_SUCCESS && poll_calls == 3);
    assert(children.active == 0);
    int owned[] = {accepted_fd, control_fds[0], control_fds[1]};
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i)
        if (owned[i] >= 0)
            assert(fcntl(owned[i], F_GETFD) == -1 && errno == EBADF);
    close(witness[0]);
    close(witness[1]);
    failure_stage = 0;
}

static void test_accept_resource_failures_pause_without_stopping(void)
{
    const int errors[] = {EMFILE, ENFILE, ENOMEM, ENOBUFS};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
        run_resource_failure(1, errors[i]);
}

static void test_control_resource_failures_close_client_and_pause(void)
{
    const int errors[] = {EMFILE, ENFILE, ENOMEM, ENOBUFS};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i)
        run_resource_failure(2, errors[i]);
}

static void test_fork_resource_failures_close_client_and_control_fds(void)
{
    run_resource_failure(3, EAGAIN);
    run_resource_failure(3, ENOMEM);
}

static void test_unexpected_admission_errors_remain_fatal(void)
{
    for (int stage = 1; stage <= 3; ++stage)
        run_resource_failure(stage, EINVAL);
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
        TEST_CASE(test_accept_resource_failures_pause_without_stopping),
        TEST_CASE(test_control_resource_failures_close_client_and_pause),
        TEST_CASE(test_fork_resource_failures_close_client_and_control_fds),
        TEST_CASE(test_unexpected_admission_errors_remain_fatal),
        TEST_CASE(test_reap_retries_interruption_and_removes_exited_pid),
        TEST_CASE(test_reap_accepts_no_children),
        TEST_CASE(test_reap_reports_unexpected_error),
        TEST_CASE(test_signal_targets_active_child),
        TEST_CASE(test_signal_accepts_already_exited_child),
        TEST_CASE(test_signal_continues_after_permission_error),
    };
    return run_tests(argc, argv, tests, sizeof(tests) / sizeof(tests[0]));
}
