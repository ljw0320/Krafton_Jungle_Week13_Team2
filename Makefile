CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
HTTP_SOURCES = http/http.c http/http_parse.c http/http_io.c http/http_response.c http/http_static.c http/http_cgi.c
HTTP_HEADERS = http/http.h http/http_internal.h
SERVER_SOURCES = server/server.c
SERVER_HEADERS = server/server.h

.PHONY: all test sanitize clean
all: build/web-server build/adder

build/adder: cgi/adder.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

build/web-server: main.c $(SERVER_SOURCES) $(SERVER_HEADERS) $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) main.c $(SERVER_SOURCES) $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-processes: tests/test_processes.c $(SERVER_SOURCES) $(SERVER_HEADERS) $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_processes.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-http: tests/test_http.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_http.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-cgi-io: tests/test_cgi_io.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_cgi_io.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

test: all build/test-processes build/test-http build/test-cgi-io
	./build/test-processes
	./build/test-http
	python3 tests/test_server.py
	python3 tests/test_cgi.py

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -f build/web-server build/adder build/test-processes build/test-http build/test-cgi-io
