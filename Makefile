CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -Wno-deprecated-declarations
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LIBS = -lcrypto
else
LIBS =
endif

all: tracker peer_client peer_server

tracker: tracker.c utils.c protocol.c
	$(CC) $(CFLAGS) tracker.c utils.c protocol.c -o tracker $(LIBS)

peer_client: peer_client.c downloader.c utils.c protocol.c
	$(CC) $(CFLAGS) peer_client.c downloader.c utils.c protocol.c -o peer_client $(LIBS)

peer_server: peer_server.c utils.c
	$(CC) $(CFLAGS) peer_server.c utils.c -o peer_server

clean:
	rm -f tracker peer_client peer_server *.o
