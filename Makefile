CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2

.PHONY: all test sanitize clean
all: build/tcp-echo

build/tcp-echo: server.c
	mkdir -p build
	$(CC) $(CFLAGS) server.c -o $@ $(LDFLAGS)

build/test-processes: tests/test_processes.c server.c
	mkdir -p build
	$(CC) $(CFLAGS) tests/test_processes.c -o $@ $(LDFLAGS)

test: all build/test-processes
	./build/test-processes
	python3 tests/test_server.py

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-std=c11 -Wall -Wextra -Wpedantic -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -f build/tcp-echo build/test-processes
