#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "downloader.h"
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

/* Resolve the host, then try each returned address until TCP connects. */
static int connect_peer(const char *host, uint16_t port) {
    struct addrinfo hints = {0}, *addresses = NULL;
    char service[6];
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

/* TCP may accept only part of a request in one send(). */
static int send_all(int fd, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* TCP is a byte stream: loop until exactly size bytes arrive. */
static int receive_exact(int fd, void *data, size_t size) {
    size_t received = 0;
    while (received < size) {
        ssize_t n = recv(fd, (char *)data + received, size - received, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1; /* Zero means the peer closed early. */
        received += (size_t)n;
    }
    return 0;
}

static int write_at(int fd, const void *data, size_t size, uint64_t offset) {
    size_t written = 0;
    while (written < size) {
        ssize_t n = pwrite(fd, (const char *)data + written, size - written,
                           (off_t)(offset + written));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        written += (size_t)n;
    }
    return 0;
}

/* One connection per chunk, with a newline header followed by binary bytes. */
static int fetch_chunk(const dl_options *o, int output, uint64_t offset, size_t length) {
    char request[1025], header[80], expected[80];
    int n = snprintf(request, sizeof(request), "GET_CHUNK %s %" PRIu64 " %zu\n",
                     o->remote_name, offset, length);
    if (n < 0 || (size_t)n >= sizeof(request)) return -1;
    int peer = connect_peer(o->host, o->port);
    if (peer < 0) return -1;
    int result = -1;
    if (send_all(peer, request, (size_t)n)) goto done;
    size_t h = 0;
    do {
        if (h == sizeof(header) - 1 || receive_exact(peer, header + h, 1)) goto done;
    } while (header[h++] != '\n');
    header[h] = '\0';
    snprintf(expected, sizeof(expected), "CHUNK %zu\n", length);
    if (strcmp(header, expected)) goto done;
    unsigned char buffer[65536];
    size_t received = 0;
    while (received < length) {
        size_t amount = length - received;
        if (amount > sizeof(buffer)) amount = sizeof(buffer);
        if (receive_exact(peer, buffer, amount) ||
            write_at(output, buffer, amount, offset + received)) goto done;
        received += amount;
    }
    result = 0;
done:
    close(peer);
    return result;
}

int dl_download(const dl_options *o, char *error, size_t error_size) {
    if (!o || !o->host || !*o->host || !o->port || !o->remote_name ||
        !*o->remote_name || !o->output_path || !*o->output_path ||
        !o->chunk_size || o->file_size > INT64_MAX)
        return fail(error, error_size, "Invalid download options");
    for (const unsigned char *p = (const unsigned char *)o->remote_name; *p; ++p)
        if (*p <= 32 || *p >= 127)
            return fail(error, error_size, "Remote name must be printable ASCII without spaces");
    char probe[1025];
    int n = snprintf(probe, sizeof(probe), "GET_CHUNK %s %" PRIu64 " %zu\n",
                     o->remote_name, o->file_size, o->chunk_size);
    if (n < 0 || (size_t)n >= sizeof(probe))
        return fail(error, error_size, "Request exceeds 1024 bytes");
    int output = open(o->output_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (output < 0) return fail(error, error_size, "Cannot create output (possibly already exists)");
    for (uint64_t offset = 0; offset < o->file_size;) {
        uint64_t remaining = o->file_size - offset;
        size_t length = remaining < o->chunk_size ? (size_t)remaining : o->chunk_size;
        if (fetch_chunk(o, output, offset, length)) {
            close(output);
            return fail(error, error_size, "Chunk transfer/write failed; output is incomplete");
        }
        offset += length;
    }
    if (close(output)) return fail(error, error_size, "Closing output failed");
    if (error && error_size) error[0] = '\0';
    return 0;
}
