CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDLIBS = -pthread
SOURCES = $(wildcard src/*.c)
HEADERS = src/server.h src/http.h

.PHONY: all test clean
all: webserver

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDLIBS)

test_http: src/tests/test_http.c src/http.c src/files.c src/http.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -Isrc src/tests/test_http.c src/http.c src/files.c -o $@

test: test_http
	./test_http www

clean:
	rm -f webserver test_http
