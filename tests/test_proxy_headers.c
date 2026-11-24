#include "../proxy/headers.h"
#include "../proxy/routes.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    struct request request = {.http11 = true};
    char raw[HEADER_LIMIT + 1];
    const char *start = "HTTP/1.1 200 OK\r\nContent-Length:0\r\n";
    strcpy(raw, start);
    size_t used = strlen(raw);
    while (used + 6 <= HEADER_LIMIT) {
        memcpy(raw + used, "X:y\r\n", 5);
        used += 5;
    }
    memcpy(raw + used, "\r\n", 3);
    char *out = NULL;
    uintmax_t length;
    assert(proxy_response_header(raw, &request, &out, &length) == 0);
    assert(length == 0);
    free(out);
    puts("proxy header tests passed");
    return 0;
}
