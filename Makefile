.PHONY: all server asan clean
CFLAGS = -std=gnu11 -Wall -Wextra -g
all: server
server:
	gcc $(CFLAGS) -o server src/*.c
asan:
	gcc $(CFLAGS) -fsanitize=address,undefined -o server src/*.c
clean:
	rm -f server
