CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2

all: tracker peer_client peer_server

tracker: tracker.c utils.c protocol.c
	$(CC) $(CFLAGS) tracker.c utils.c protocol.c -o tracker

peer_client: peer_client.c downloader.c utils.c protocol.c
	$(CC) $(CFLAGS) peer_client.c downloader.c utils.c protocol.c -o peer_client

peer_server: peer_server.c utils.c
	$(CC) $(CFLAGS) peer_server.c utils.c -o peer_server

clean:
	rm -f tracker peer_client peer_server *.o
