CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2 -Wno-deprecated-declarations
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
LIBS = -lcrypto
else
LIBS =
endif

PEER_DIRS = peer1 peer2 peer3

all: tracker peer_client peer_server peers

tracker: tracker.c utils.c protocol.c
	$(CC) $(CFLAGS) tracker.c utils.c protocol.c -o tracker $(LIBS)

peer_client: peer_client.c downloader.c downloader.h utils.c protocol.c
	$(CC) $(CFLAGS) peer_client.c downloader.c utils.c protocol.c -o peer_client $(LIBS)

peer_server: peer_server.c downloader.c downloader.h utils.c protocol.c
	$(CC) $(CFLAGS) peer_server.c downloader.c utils.c protocol.c -o peer_server $(LIBS)

peers: peer_client peer_server
	@for d in $(PEER_DIRS); do \
		mkdir -p "$$d"; \
		cp peer_client peer_server "$$d/"; \
	done

clean:
	rm -f tracker peer_client peer_server downloader_cli *.o
	@for d in $(PEER_DIRS); do \
		rm -f "$$d/peer_client" "$$d/peer_server" "$$d/*.o"; \
	done