.PHONY: all server asan clean test
CFLAGS = -std=gnu11 -Wall -Wextra -g
all: server
server:
	gcc $(CFLAGS) -o server src/*.c
asan:
	gcc $(CFLAGS) -fsanitize=address,undefined -o server src/*.c
test: server
	gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && bash tests/http_test.sh
clean:
	rm -f server tests/test_http
