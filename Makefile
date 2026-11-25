.PHONY: all server asan clean test
CFLAGS = -std=gnu11 -Wall -Wextra -g
all: server
server:
	gcc $(CFLAGS) -o server src/*.c
asan:
	gcc $(CFLAGS) -fsanitize=address,undefined -o server src/*.c
test: server
	gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && gcc $(CFLAGS) -o tests/test_proxy tests/test_proxy.c && ./tests/test_proxy && bash tests/http_test.sh && bash tests/proxy_test.sh
	bash tests/chat_test.sh
clean:
	rm -f server tests/test_http tests/test_proxy
