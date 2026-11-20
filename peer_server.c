#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <sys/time.h>
#include "utils.h"

#define PORT 9001
#define MAX_REQUEST_SIZE 1024
#define MAX_CHUNK_SIZE 1024

static int send_all(int fd, const char *data, size_t length) {
    while (length) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return -1;
        data += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int read_request(int fd, char buffer[MAX_REQUEST_SIZE + 1]) {
    size_t used = 0;
    while (used < MAX_REQUEST_SIZE) {
        ssize_t received = recv(fd, buffer + used, MAX_REQUEST_SIZE - used, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return -1;
        char *newline = memchr(buffer + used, '\n', (size_t)received);
        if (newline) {
            if (memchr(buffer, '\0', (size_t)(newline - buffer))) return -1;
            *newline = '\0';
            return 0;
        }
        used += (size_t)received;
    }
    return -1;
}

static int parse_decimal(const char *text, uintmax_t max, uintmax_t *result) {
    char *end;
    if (*text < '0' || *text > '9') return -1;
    errno = 0;
    *result = strtoumax(text, &end, 10);
    return errno || *end || *result > max ? -1 : 0;
}

void *handle_peer_request(void *client_socket_ptr) {
    int client_sock = *(int *)client_socket_ptr;
    free(client_socket_ptr);

    char buffer[MAX_REQUEST_SIZE + 1];
    const struct timeval timeout = {3, 0};
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char filename[256], offset_text[21], length_text[21], extra;
    uintmax_t offset, length;

    if (!read_request(client_sock, buffer) &&
        sscanf(buffer, "GET_CHUNK %255s %20s %20s %c", filename, offset_text,
               length_text, &extra) == 3 &&
        !strchr(filename, '/') && !strchr(filename, '\\') &&
        strcmp(filename, ".") && strcmp(filename, "..") &&
        !parse_decimal(offset_text, LONG_MAX, &offset) &&
        !parse_decimal(length_text, MAX_CHUNK_SIZE, &length) && length) {
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "./shared_files/%s", filename);

        char chunk_buffer[MAX_CHUNK_SIZE];
        int bytes_read = readChunk(file_path, (long)offset, chunk_buffer, (size_t)length);
        if (bytes_read == (int)length) {
            char header[80];
            int hdr_len = snprintf(header, sizeof(header), "CHUNK %d\n", bytes_read);
            if (!send_all(client_sock, header, (size_t)hdr_len))
                send_all(client_sock, chunk_buffer, (size_t)bytes_read);
        } else {
            const char *err = "ERROR: Failed to read file chunk\n";
            send_all(client_sock, err, strlen(err));
        }
    } else {
        const char *err = "ERROR: Invalid request format\n";
        send_all(client_sock, err, strlen(err));
    }

    close(client_sock);
    return NULL;
}

int main() {
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Ensure shared folder exists
    initSharedFolder("./shared_files");

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    if (listen(server_sock, 10) < 0) {
        perror("Listen failed");
        close(server_sock);
        exit(EXIT_FAILURE);
    }

    printf("[PeerServer] Running and listening on port %d...\n", PORT);

    while (1) {
        int *new_sock = malloc(sizeof(int));
        if (!new_sock) {
            perror("Allocating client socket");
            break;
        }
        *new_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (*new_sock < 0) {
            perror("Accept failed");
            free(new_sock);
            continue;
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_peer_request, (void *)new_sock) != 0) {
            perror("Thread creation failed");
            close(*new_sock);
            free(new_sock);
            continue;
        }
        pthread_detach(thread_id);
    }

    close(server_sock);
    return 0;
}
