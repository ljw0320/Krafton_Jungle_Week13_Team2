.PHONY: all server chatd livehub asan clean test load
CFLAGS = -std=gnu11 -Wall -Wextra -g
all: server chatd livehub
server:
	gcc $(CFLAGS) -o server src/*.c
chatd:
	gcc $(CFLAGS) -o chatd chat/chatd.c chat/room.c chat/ws.c chat/live_wire.c src/http.c src/server.c
livehub:
	gcc $(CFLAGS) -o livehub chat/livehub.c chat/live_wire.c src/server.c
asan: CFLAGS += -fsanitize=address,undefined
asan: all
test: all
	gcc $(CFLAGS) -o tests/test_ws tests/test_ws.c chat/room.c && ./tests/test_ws
	gcc $(CFLAGS) -o tests/test_http tests/test_http.c && ./tests/test_http && gcc $(CFLAGS) -o tests/test_proxy tests/test_proxy.c && ./tests/test_proxy && bash tests/http_test.sh && bash tests/proxy_test.sh
	bash tests/chat_test.sh
	python3 -B tests/live_room.py
	python3 -B tools/test_live_demo.py
load: all
	bash tests/chat_test.sh load
clean:
	rm -f server chatd livehub tests/test_http tests/test_proxy tests/test_ws
