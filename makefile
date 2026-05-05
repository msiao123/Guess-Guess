CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread

.PHONY: build run-server run-client test clean

build: server client

server: server.c
	$(CC) $(CFLAGS) -o server server.c

client: client.c
	$(CC) $(CFLAGS) -o client client.c

run-server: build
	./server

run-client: build
	./client

test:
	@echo "No tests yet. (Will be added later.)"

clean:
	rm -f server client

