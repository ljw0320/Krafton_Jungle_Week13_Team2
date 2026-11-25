.PHONY: all server chatd asan clean test load
CFLAGS = -std=gnu11 -Wall -Wextra -g
all: server chatd
server:
	gcc $(CFLAGS) -o server src/*.c
chatd:
	gcc $(CFLAGS) -o chatd chat/*.c src/http.c src/server.c
asan: CFLAGS += -fsanitize=address,undefined
asan: all
test: all
	gcc $(CFLAGS) -o tests/test_ws tests/test_ws.c chat/room.c && ./tests/test_ws
	gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && gcc $(CFLAGS) -o tests/test_proxy tests/test_proxy.c && ./tests/test_proxy && bash tests/http_test.sh && bash tests/proxy_test.sh
	bash tests/chat_test.sh
load: all
	bash tests/chat_test.sh load
clean:
	rm -f server chatd tests/test_http tests/test_proxy tests/test_ws
