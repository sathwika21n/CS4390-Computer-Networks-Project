CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
LIBS = -lcrypto

all: tracker peer_client peer_server

tracker: tracker.c utils.c
	$(CC) $(CFLAGS) tracker.c utils.c -o tracker $(LIBS)

peer_client: peer_client.c downloader.c utils.c
	$(CC) $(CFLAGS) peer_client.c downloader.c utils.c -o peer_client $(LIBS)

peer_server: peer_server.c utils.c
	$(CC) $(CFLAGS) peer_server.c utils.c -o peer_server $(LIBS)

clean:
	rm -f tracker peer_client peer_server *.o