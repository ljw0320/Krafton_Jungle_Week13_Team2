CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
HTTP_SOURCES = http.c http_parse.c http_io.c http_response.c http_static.c
HTTP_HEADERS = http.h http_internal.h

.PHONY: all test sanitize clean
all: build/web-server

build/web-server: server.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) server.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-processes: tests/test_processes.c server.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_processes.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-http: tests/test_http.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_http.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

test: all build/test-processes build/test-http
	./build/test-processes
	./build/test-http
	python3 tests/test_server.py

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -f build/web-server build/test-processes build/test-http
