CC = cc
CPPFLAGS = -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDLIBS = -pthread
SOURCES = $(wildcard src/*.c)
HEADERS = src/server.h src/http.h

.PHONY: all test test_cgi clean
all: webserver cgi-bin/adder

cgi-bin/adder: src/cgi-bin/adder.c
	mkdir -p cgi-bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $< -o $@

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

test_http: src/tests/test_http.c src/http.c src/files.c src/http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_http.c src/http.c src/files.c -o $@

CONNECTION_SOURCES = src/connection.c src/platform.c src/http.c src/files.c src/cgi.c src/cgi_spawn.c src/cgi_response.c

test_drain: src/tests/test_drain.c $(CONNECTION_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_drain.c $(CONNECTION_SOURCES) -o $@ $(LDLIBS)

test: test_http test_drain
	./test_http www
	./test_drain

test_cgi: all
	python3 src/tests/test_cgi.py

clean:
	rm -f webserver test_http test_drain cgi-bin/adder
