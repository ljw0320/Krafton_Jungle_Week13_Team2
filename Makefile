CC = cc
CPPFLAGS = -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDLIBS = -pthread
SOURCES = $(wildcard src/*.c)
HEADERS = src/server.h src/server_profile.h src/http.h common/runtime.h

.PHONY: all test test_stage1 test_stage2 test_cgi test_keep_alive check_keep_alive test_proxy clean
all: webserver cgi-bin/adder reverse_proxy

cgi-bin/adder: src/cgi-bin/adder.c
	mkdir -p cgi-bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

test_http: src/tests/test_http.c src/http.c src/files.c src/http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_http.c src/http.c src/files.c -o $@

CONNECTION_SOURCES = src/server_profile.c src/resources.c src/connection.c src/context_pool.c src/platform.c src/http.c src/files.c src/cgi.c src/cgi_spawn.c src/cgi_response.c

test_drain: src/tests/test_drain.c $(CONNECTION_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_drain.c $(CONNECTION_SOURCES) -o $@ $(LDLIBS)

CONTEXT_TEST_SOURCES = $(filter-out src/connection.c,$(CONNECTION_SOURCES)) src/thread_pool.c src/timers.c

test_context: src/tests/test_support.h src/tests/context_fixture.h src/tests/test_context.c src/server.c src/connection.c $(CONTEXT_TEST_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_context.c $(CONTEXT_TEST_SOURCES) -o $@ $(LDLIBS)

test_stage1: test_context
	./test_context

test_context_pool: src/tests/test_context_pool.c src/context_pool.c src/platform.c $(HEADERS) src/tests/test_support.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_context_pool.c src/context_pool.c src/platform.c -o $@ $(LDLIBS)

test_stage2: test_context_pool test_context
	./test_context_pool
	./test_context

test: test_http test_drain test_context test_context_pool
	./test_http www
	./test_drain
	./test_context_pool
	./test_context

test_cgi: all
	python3 src/tests/test_cgi.py

test_keep_alive: all
	python3 src/tests/test_keep_alive.py

check_keep_alive: all
	python3 src/tests/run_keep_alive_checks.py --output keep-alive-report.json

clean:
	rm -f test_timer_heap test_server_epoll test_proxy_reactor webserver reverse_proxy test_proxy_headers test_proxy_state test_http test_drain test_context test_context_pool cgi-bin/adder

PROXY_SOURCES = proxy/main.c proxy/headers.c src/http.c
reverse_proxy: $(PROXY_SOURCES) proxy/proxy.h proxy/reactor.h proxy/profile.h src/http.h common/runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc $(PROXY_SOURCES) -o $@

test_proxy_headers: proxy/tests/test_headers.c proxy/headers.c proxy/proxy.h src/http.c src/http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -Iproxy proxy/tests/test_headers.c proxy/headers.c src/http.c -o $@

test_proxy: reverse_proxy test_proxy_headers test_proxy_state test_proxy_reactor all
	./test_proxy_headers
	./test_proxy_state
	./test_proxy_reactor
	python3 proxy/tests/test_proxy.py

test_scheduler: src/tests/test_scheduler.c src/server.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_scheduler.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c -o $@ $(LDLIBS)

test_proxy_state: proxy/tests/test_state.c $(PROXY_SOURCES) proxy/proxy.h proxy/reactor.h proxy/profile.h src/http.h common/runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -Iproxy proxy/tests/test_state.c proxy/headers.c src/http.c -o $@

test_proxy_reactor: proxy/tests/test_reactor.c proxy/proxy.h proxy/reactor.h common/runtime.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc -Iproxy proxy/tests/test_reactor.c -o $@

# Real epoll sockets and the existing scheduler/worker implementation.
test_server_epoll: src/tests/test_server_epoll.c src/server.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_server_epoll.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c -o $@ $(LDLIBS)

test_timer_heap: src/tests/test_timer_heap.c src/server.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_timer_heap.c $(CONNECTION_SOURCES) src/thread_pool.c src/timers.c -o $@ $(LDLIBS)
