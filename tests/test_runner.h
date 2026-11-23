#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct test_case {
    const char *name;
    void (*run)(void);
};

#define TEST_CASE(function) {#function, function}

/* Isolate assertions and globals so one failure does not hide later tests. */
static int run_tests(int argc, char **argv, const struct test_case *tests, size_t count)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [test_name]\n", argv[0]);
        return EXIT_FAILURE;
    }
    size_t selected = 0, failed = 0;
    for (size_t i = 0; i < count; ++i) {
        if (argc == 2 && strcmp(argv[1], tests[i].name) != 0)
            continue;
        ++selected;
        fflush(NULL);
        pid_t pid = fork();
        if (pid == -1) {
            perror("fork test");
            return EXIT_FAILURE;
        }
        if (pid == 0) {
            tests[i].run();
            exit(EXIT_SUCCESS);
        }
        int status;
        pid_t result;
        do { result = waitpid(pid, &status, 0); } while (result == -1 && errno == EINTR);
        if (result == -1) {
            perror("waitpid test");
            return EXIT_FAILURE;
        }
        int passed = WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS;
        printf("%s: %s\n", passed ? "PASS" : "FAIL", tests[i].name);
        failed += !passed;
    }
    if (!selected)
        fprintf(stderr, "Unknown test: %s\n", argv[1]);
    return selected && !failed ? EXIT_SUCCESS : EXIT_FAILURE;
}

#endif
