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
#include "downloader.h"

#define PORT 9001
#define MAX_REQUEST_SIZE 1024
#define MAX_CHUNK_SIZE 1024

static unsigned short listen_port = PORT;
static char shared_directory[512] = "./shared_files";
static char download_directory[512] = "./downloads";

static int parse_decimal(const char *text, uintmax_t max, uintmax_t *result);

static int load_server_config(const char *path, int required) {
    FILE *file = fopen(path, "r");
    if (!file) return !required && errno == ENOENT ? 0 : -1;
    char line[1024];
    int result = 0;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *value = strchr(line, '=');
        if (!value || line[0] == '#') continue;
        *value++ = '\0';
        if (!strcmp(line, "PORT")) {
            uintmax_t port;
            if (parse_decimal(value, UINT16_MAX, &port) || !port) {
                result = -1;
                break;
            }
            listen_port = (unsigned short)port;
        } else if (!strcmp(line, "SHARED_DIR") || !strcmp(line, "DOWNLOAD_DIR")) {
            if (!*value || strlen(value) >= sizeof(shared_directory)) {
                result = -1;
                break;
            }
            strcpy(!strcmp(line, "SHARED_DIR") ? shared_directory : download_directory, value);
        }
    }
    if (ferror(file)) result = -1;
    fclose(file);
    return result;
}

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
    char filename[256], offset_text[21], length_text[MAX_REQUEST_SIZE + 1], extra;
    uintmax_t offset, length;

    if (!read_request(client_sock, buffer) &&
        sscanf(buffer, "GET_CHUNK %255s %20s %1024s %c", filename, offset_text,
               length_text, &extra) == 3 &&
        !strchr(filename, '/') && !strchr(filename, '\\') &&
        strcmp(filename, ".") && strcmp(filename, "..") &&
        !parse_decimal(offset_text, LONG_MAX, &offset) &&
        length_text[strspn(length_text, "0123456789")] == '\0' &&
        (length = strtoumax(length_text, NULL, 10)) != 0) {
        /* An overflowing decimal length saturates and is also oversized. */
        if (length > MAX_CHUNK_SIZE) {
            const char invalid[] = "<GET invalid>\n";
            send_all(client_sock, invalid, sizeof(invalid) - 1);
            close(client_sock);
            return NULL;
        }
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s", shared_directory, filename);

        char chunk_buffer[MAX_CHUNK_SIZE];
        int bytes_read = readChunk(file_path, (long)offset, chunk_buffer, (size_t)length);
        if (bytes_read != (int)length &&
            !dl_read_partial(download_directory, filename, offset, chunk_buffer, (size_t)length))
            bytes_read = (int)length;
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

int main(int argc, char **argv) {
    if (argc > 2 || load_server_config(argc == 2 ? argv[1] : "serverThreadConfig.cfg", argc == 2)) {
        fprintf(stderr, "Usage: %s [valid serverThreadConfig.cfg]\n", argv[0]);
        return EXIT_FAILURE;
    }
    int server_sock;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // Ensure shared folder exists
    initSharedFolder(shared_directory);

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(listen_port);

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

    printf("[PeerServer] Running and listening on port %u...\n", (unsigned)listen_port);

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
