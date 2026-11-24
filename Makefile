CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
HTTP_SOURCES = http/http.c http/http_parse.c http/http_io.c http/http_response.c http/http_static.c http/http_cgi.c proxy/routes.c proxy/headers.c proxy/proxy.c log/diagnostics.c
HTTP_HEADERS = http/http.h http/http_internal.h proxy/routes.h proxy/headers.h proxy/proxy.h log/diagnostics.h
SERVER_SOURCES = server/server.c
SERVER_HEADERS = server/server.h

.PHONY: all test sanitize smoke-proxy clean
all: build/web-server build/adder

build/adder: cgi/adder.c
	mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

build/web-server: main.c $(SERVER_SOURCES) $(SERVER_HEADERS) $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) main.c $(SERVER_SOURCES) $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-processes: tests/test_processes.c tests/test_runner.h $(SERVER_SOURCES) $(SERVER_HEADERS) $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_processes.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-http: tests/test_http.c tests/test_runner.h $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_http.c $(HTTP_SOURCES) -o $@ $(LDFLAGS)

build/test-cgi-io: tests/test_cgi_io.c $(HTTP_SOURCES) $(HTTP_HEADERS)
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_cgi_io.c $(HTTP_SOURCES) -o $@ $(LDFLAGS) -Wl,--wrap=write

build/test-proxy-headers: tests/test_proxy_headers.c proxy/headers.c proxy/headers.h http/http_internal.h
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_proxy_headers.c proxy/headers.c -o $@ $(LDFLAGS)

build/test-proxy-io: tests/test_proxy_io.c proxy/proxy.c proxy/headers.c $(HTTP_HEADERS) http/http_response.c http/http_io.c log/diagnostics.c
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_proxy_io.c proxy/headers.c http/http_response.c http/http_io.c log/diagnostics.c -o $@ $(LDFLAGS) -Wl,--wrap=poll -Wl,--wrap=__poll_chk -Wl,--wrap=connect

test: all build/test-processes build/test-http build/test-cgi-io build/test-proxy-headers build/test-proxy-io
	./build/test-processes
	./build/test-http
	./build/test-proxy-headers
	./build/test-proxy-io
	python3 tests/test_server.py
	python3 tests/test_cgi.py
	python3 tests/test_proxy.py

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

smoke-proxy: all
	python3 tests/smoke_proxy.py

clean:
	rm -f build/web-server build/adder build/test-processes build/test-http build/test-cgi-io build/test-proxy-headers build/test-proxy-io
