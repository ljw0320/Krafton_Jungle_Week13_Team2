#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_LIMIT = 8192, POST_LIMIT = 4096 };

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode each field after splitting, so encoded separators remain field data. */
static bool decode_field(char *text)
{
    char *out = text;
    for (char *p = text; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '%') {
            if (!p[1] || !p[2])
                return false;
            int high = hex_digit((unsigned char)p[1]);
            int low = hex_digit((unsigned char)p[2]);
            if (high < 0 || low < 0)
                return false;
            c = (unsigned char)(high * 16 + low);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c < 32 || c >= 127)
            return false;
        *out++ = (char)c;
    }
    *out = '\0';
    return true;
}

static bool parse_integer(const char *text, int64_t *value)
{
    const char *digits = text;
    if (*digits == '-' || *digits == '+')
        ++digits;
    if (!*digits)
        return false;
    for (const char *p = digits; *p; ++p)
        if (*p < '0' || *p > '9')
            return false;
    errno = 0;
    intmax_t number = strtoimax(text, NULL, 10);
    if (errno == ERANGE || number < INT64_MIN || number > INT64_MAX)
        return false;
    *value = (int64_t)number;
    return true;
}

static bool add_form(char *form, int64_t *sum)
{
    bool seen[2] = {false, false};
    int64_t values[2] = {0, 0};
    for (char *field = form; ; ) {
        char *next = strchr(field, '&');
        if (next)
            *next++ = '\0';
        char *equal = strchr(field, '=');
        if (!equal)
            return false;
        *equal++ = '\0';
        if (!decode_field(field) || !decode_field(equal))
            return false;
        int index = strcmp(field, "a") == 0 ? 0 : strcmp(field, "b") == 0 ? 1 : -1;
        if (index < 0 || seen[index] || !parse_integer(equal, &values[index]))
            return false;
        seen[index] = true;
        if (!next)
            break;
        field = next;
    }
    if (!seen[0] || !seen[1] ||
        (values[1] > 0 && values[0] > INT64_MAX - values[1]) ||
        (values[1] < 0 && values[0] < INT64_MIN - values[1]))
        return false;
    *sum = values[0] + values[1];
    return true;
}

static bool read_input(char *input)
{
    const char *method = getenv("REQUEST_METHOD");
    if (method && strcmp(method, "GET") == 0) {
        const char *query = getenv("QUERY_STRING");
        if (!query || strlen(query) > INPUT_LIMIT)
            return false;
        strcpy(input, query);
        return true;
    }
    if (!method || strcmp(method, "POST") != 0)
        return false;
    const char *type = getenv("CONTENT_TYPE");
    const char *length = getenv("CONTENT_LENGTH");
    if (!type || strcmp(type, "application/x-www-form-urlencoded") != 0 || !length || !*length)
        return false;
    size_t n = 0;
    for (const char *p = length; *p; ++p) {
        if (*p < '0' || *p > '9' || n > POST_LIMIT / 10)
            return false;
        n = n * 10 + (size_t)(*p - '0');
        if (n > POST_LIMIT)
            return false;
    }
    if (fread(input, 1, n, stdin) != n || memchr(input, '\0', n))
        return false;
    input[n] = '\0';
    return true;
}

int main(void)
{
    char input[INPUT_LIMIT + 1];
    int64_t sum;
    if (!read_input(input) || !add_form(input, &sum)) {
        printf("Status: 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\n\r\n"
               "Expected a and b as signed 64-bit integers with a representable sum.\n");
    } else {
        printf("Content-Type: text/plain; charset=utf-8\r\n\r\n%" PRId64 "\n", sum);
    }
    return ferror(stdout) || fflush(stdout) == EOF ? EXIT_FAILURE : EXIT_SUCCESS;
}
