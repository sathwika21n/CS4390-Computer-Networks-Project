#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>
#include "utils.h"

#define MAX_REQUEST_SIZE 1024

static unsigned short peer_port = 3490;
static char shared_dir[512] = "./shared";

static char *trim(char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) *--end = '\0';
    return text;
}

static int read_config(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1;
    }
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        char *key = trim(line);
        char *comment = strchr(key, '#');
        if (comment) *comment = '\0';
        key = trim(key);
        if (!*key) continue;
        char *separator = strchr(key, '=');
        if (!separator) continue;
        *separator = '\0';
        char *value = trim(separator + 1);
        key = trim(key);
        if (strcmp(key, "PORT") == 0) {
            char *end = NULL;
            unsigned long port = strtoul(value, &end, 10);
            if (end == value || *end || port == 0 || port > 65535) {
                fprintf(stderr, "Invalid peer server PORT in %s\n", path);
                fclose(file);
                return -1;
            }
            peer_port = (unsigned short)port;
        } else if (strcmp(key, "SHARED_DIR") == 0) {
            snprintf(shared_dir, sizeof(shared_dir), "%s", value);
        }
    }
    fclose(file);
    return 0;
}

void *handle_peer_request(void *client_socket_ptr) {
    int client_sock = *(int *)client_socket_ptr;
    free(client_socket_ptr);

    char buffer[MAX_REQUEST_SIZE + 1];
    memset(buffer, 0, sizeof(buffer));

    // Read incoming request
    ssize_t bytes_received = recv(client_sock, buffer, sizeof(buffer), 0);
    if (bytes_received < 0) {
        perror("Receive failed");
        close(client_sock);
        return NULL;
    }

    // Requirement: Reject requests over 1024 bytes
    if (bytes_received > MAX_REQUEST_SIZE) {
        printf("[PeerServer] Warning: Request exceeded 1024 bytes. Rejecting.\n");
        char *error_msg = "ERROR: Request too large (>1024 bytes)\n";
        send(client_sock, error_msg, strlen(error_msg), 0);
        close(client_sock);
        return NULL;
    }

    buffer[bytes_received] = '\0';
    printf("[PeerServer] Received request: %s", buffer);

    // Parse request (Expected format example: "GET filename offset size")
    char filename[256];
    uint64_t offset = 0;
    size_t chunk_size = 0;

    if (sscanf(buffer, "GET_CHUNK %255s %" SCNu64 " %zu", filename, &offset, &chunk_size) == 3) {
        char file_path[512];
        snprintf(file_path, sizeof(file_path), "%s/%s", shared_dir, filename);

        char *chunk_buffer = malloc(chunk_size);
        if (!chunk_buffer) {
            perror("Memory allocation failed");
            close(client_sock);
            return NULL;
        }

        int bytes_read = readChunk(file_path, offset, chunk_buffer, chunk_size);
        if (bytes_read > 0) {
            char header[80];
            int hdr_len = snprintf(header, sizeof(header), "CHUNK %d\n", bytes_read);
            send(client_sock, header, hdr_len, 0);

            send(client_sock, chunk_buffer, bytes_read, 0);
        } else {
            char *err = "ERROR: Failed to read file chunk\n";
            send(client_sock, err, strlen(err), 0);
        }

        free(chunk_buffer);
    } else {
        char *err = "ERROR: Invalid request format\n";
        send(client_sock, err, strlen(err), 0);
    }

    close(client_sock);
    return NULL;
}

int main(void) {
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Read configuration
    if (read_config("serverThreadConfig.cfg") != 0) {
        fprintf(stderr, "Failed to read configuration. Exiting.\n");
        return EXIT_FAILURE;
    }

    // Ensure shared folder exists
    initSharedFolder(shared_dir);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(peer_port);

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

    printf("[PeerServer] Running and listening on port %d...\n", peer_port);

    while (1) {
        int *new_sock = malloc(sizeof(int));
        *new_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
        if (*new_sock < 0) {
            perror("Accept failed");
            free(new_sock);
            continue;
        }

        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_peer_request, (void *)new_sock) != 0) {
            perror("Thread creation failed");
            free(new_sock);
        }
        pthread_detach(thread_id);
    }

    close(server_sock);
    return 0;
}
