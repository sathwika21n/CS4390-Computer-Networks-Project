#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "downloader.h"
#include "protocol.h"
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CHUNK_LIMIT = 1024, WORKERS = 4, ATTEMPT_MS = 2500 };

static int fail(char *error, size_t size, const char *message) {
    if (error && size) snprintf(error, size, "%s", message);
    return -1;
}

static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_socket(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline) return -1;
        struct pollfd p = {.fd = fd, .events = events};
        int ready = poll(&p, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 ? 0 : -1;
    }
}

/* Resolve the host, then try addresses within the same attempt deadline. */
static int connect_peer(const char *host, uint16_t port, int64_t deadline, int numeric) {
    struct addrinfo hints = {0}, *addresses = NULL;
    char service[6];
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (numeric) hints.ai_flags = AI_NUMERICHOST;
    snprintf(service, sizeof(service), "%u", (unsigned)port);
    if (getaddrinfo(host, service, &hints, &addresses)) return -1;
    int fd = -1;
    for (struct addrinfo *a = addresses; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
            int status = connect(fd, a->ai_addr, a->ai_addrlen);
            if (status == 0) break;
            if (errno == EINPROGRESS && wait_socket(fd, POLLOUT, deadline) == 0) {
                int error;
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 &&
                    error == 0) break;
            }
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    return fd;
}

/* TCP may accept only part of a request in one send(). */
static int send_all(int fd, const char *data, size_t size, int64_t deadline) {
    size_t sent = 0;
    while (sent < size) {
        if (wait_socket(fd, POLLOUT, deadline)) return -1;
        ssize_t n = send(fd, data + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* TCP is a byte stream: loop until exactly size bytes arrive. */
static int receive_exact(int fd, void *data, size_t size, int64_t deadline) {
    size_t received = 0;
    while (received < size) {
        if (wait_socket(fd, POLLIN, deadline)) return -1;
        ssize_t n = recv(fd, (char *)data + received, size - received, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
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
static const char *fetch_chunk(const char *host, uint16_t port, const char *name,
                               uint64_t offset, size_t length, void *buffer, int numeric) {
    char request[1025], header[80], expected[80];
    int n = snprintf(request, sizeof(request), "GET_CHUNK %s %" PRIu64 " %zu\n",
                     name, offset, length);
    if (n < 0 || (size_t)n >= sizeof(request)) return "request too long";
    int64_t start = monotonic_ms();
    if (start < 0) return "clock unavailable";
    int64_t deadline = start + ATTEMPT_MS;
    int peer = connect_peer(host, port, deadline, numeric);
    if (peer < 0) return "connection failed or timed out";
    const char *result = "request send failed or timed out";
    if (send_all(peer, request, (size_t)n, deadline)) goto done;
    size_t h = 0;
    result = "invalid or incomplete response header";
    do {
        if (h == sizeof(header) - 1 || receive_exact(peer, header + h, 1, deadline)) goto done;
    } while (header[h++] != '\n');
    header[h] = '\0';
    snprintf(expected, sizeof(expected), "CHUNK %zu\n", length);
    if (h != strlen(expected) || memcmp(header, expected, h)) goto done;
    result = receive_exact(peer, buffer, length, deadline) ?
        "incomplete chunk or transfer timed out" : NULL;
done:
    close(peer);
    return result;
}

int dl_download(const dl_options *o, char *error, size_t error_size) {
    if (!o || !o->host || !*o->host || !o->port || !o->remote_name ||
        !*o->remote_name || !o->output_path || !*o->output_path ||
        !o->chunk_size || o->chunk_size > CHUNK_LIMIT || o->file_size > INT64_MAX)
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
        unsigned char buffer[CHUNK_LIMIT];
        if (fetch_chunk(o->host, o->port, o->remote_name, offset, length, buffer, 0) ||
            write_at(output, buffer, length, offset)) {
            close(output);
            return fail(error, error_size, "Chunk transfer/write failed; output is incomplete");
        }
        offset += length;
    }
    if (close(output)) return fail(error, error_size, "Closing output failed");
    if (error && error_size) error[0] = '\0';
    return 0;
}


typedef struct {
    char host[INET_ADDRSTRLEN];
    uint16_t port;
    uint64_t start, end, timestamp;
    unsigned active;
} dl_peer;

typedef struct {
    char name[PROTOCOL_NAME_MAX], md5[PROTOCOL_MD5_HEX_LEN + 1];
    uint64_t size;
    dl_peer *peers;
    size_t count;
} track_file;

static int parse_uint(const char *value, char terminator, uint64_t *number,
                      const char **next) {
    char *end;
    if (*value < '0' || *value > '9') return -1;
    errno = 0;
    uintmax_t result = strtoumax(value, &end, 10);
    if (errno || result > UINT64_MAX || *end != terminator) return -1;
    *number = (uint64_t)result;
    *next = end + (terminator != '\0');
    return 0;
}

static int parse_peer(const char *line, dl_peer *peer) {
    const char *separator = strchr(line, ':');
    if (!separator || separator - line >= (ptrdiff_t)sizeof(peer->host)) return -1;
    memcpy(peer->host, line, (size_t)(separator - line));
    struct in_addr ip;
    uint64_t port;
    const char *next = separator + 1;
    if (inet_pton(AF_INET, peer->host, &ip) != 1 ||
        parse_uint(next, ':', &port, &next) || port == 0 || port > UINT16_MAX ||
        parse_uint(next, ':', &peer->start, &next) ||
        parse_uint(next, ':', &peer->end, &next) || peer->start > peer->end ||
        parse_uint(next, '\0', &peer->timestamp, &next)) return -1;
    peer->port = (uint16_t)port;
    return 0;
}

static int compare_peers(const void *left, const void *right) {
    const dl_peer *a = left, *b = right;
    if (a->timestamp != b->timestamp) return a->timestamp > b->timestamp ? -1 : 1;
    int host_order = strcmp(a->host, b->host);
    if (host_order) return host_order;
    return (a->port > b->port) - (a->port < b->port);
}

static int track_name_valid(const char *name) {
    if (!*name || strlen(name) >= PROTOCOL_NAME_MAX || !strcmp(name, ".") ||
        !strcmp(name, "..") || !strcmp(name, ".downloader") || !strcmp(name, ".trackers"))
        return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p <= 32 || *p >= 127 || strchr("/\\<>:", *p)) return 0;
    return 1;
}

static int read_track(const char *path, track_file *track) {
    FILE *input = fopen(path, "r");
    if (!input) return -1;
    char *line = NULL;
    size_t capacity = 0, peer_capacity = 0;
    unsigned fields = 0;
    ssize_t length;
    int result = -1;
    while ((length = getline(&line, &capacity, input)) >= 0) {
        if (memchr(line, '\0', (size_t)length)) goto done;
        while (length && (line[length - 1] == '\r' || line[length - 1] == '\n'))
            line[--length] = '\0';
        if (!length || *line == '#' || !strncmp(line, "Description: ", 13)) continue;
        if (!strncmp(line, "Filename: ", 10)) {
            if ((fields & 1) || !track_name_valid(line + 10)) goto done;
            strcpy(track->name, line + 10);
            fields |= 1;
        } else if (!strncmp(line, "Filesize: ", 10)) {
            const char *end;
            if ((fields & 2) || parse_uint(line + 10, '\0', &track->size, &end) ||
                track->size > INT64_MAX) goto done;
            fields |= 2;
        } else if (!strncmp(line, "MD5: ", 5)) {
            const char *hash = line + 5;
            if ((fields & 4) || strlen(hash) != PROTOCOL_MD5_HEX_LEN ||
                strspn(hash, "0123456789abcdefABCDEF") != PROTOCOL_MD5_HEX_LEN) goto done;
            for (size_t i = 0; i < PROTOCOL_MD5_HEX_LEN; ++i) {
                char c = hash[i];
                track->md5[i] = c >= 'A' && c <= 'F' ? (char)(c + ('a' - 'A')) : c;
            }
            fields |= 4;
        } else {
            dl_peer peer = {0};
            if (parse_peer(line, &peer)) goto done;
            if (track->count == peer_capacity) {
                if (peer_capacity > SIZE_MAX / (2 * sizeof(peer))) goto done;
                peer_capacity = peer_capacity ? 2 * peer_capacity : 8;
                dl_peer *peers = realloc(track->peers, peer_capacity * sizeof(peer));
                if (!peers) goto done;
                track->peers = peers;
            }
            track->peers[track->count++] = peer;
        }
    }
    if (!feof(input) || ferror(input) || fields != 7) goto done;
    /* updatetracker publishes an inclusive last byte. Existing createtracker
     * rows can use the file size itself, so clamp both forms at EOF. */
    for (size_t i = 0; i < track->count; ++i) {
        dl_peer *peer = &track->peers[i];
        peer->end = peer->end < track->size ? peer->end + 1 : track->size;
    }
    if (track->count > 1) qsort(track->peers, track->count, sizeof(dl_peer), compare_peers);
    result = 0;
done:
    free(line);
    fclose(input);
    return result;
}

enum { MAP_HEADER_SIZE = 64 };

typedef struct {
    track_file *track;
    int part_fd, map_fd;
    uint64_t next, slots, failures;
    pthread_mutex_t mutex;
} download_job;

typedef struct {
    download_job *job;
    unsigned number;
} worker_args;

static int covers(const dl_peer *peer, uint64_t start, uint64_t end) {
    return peer->start <= start && end <= peer->end;
}

/* The newest idle peer gets first choice. If every candidate is busy, try
 * the newest one anyway so a single source can serve concurrent requests. */
static size_t reserve_peer(download_job *job, uint64_t start, uint64_t end) {
    size_t first = job->track->count;
    pthread_mutex_lock(&job->mutex);
    for (size_t i = 0; i < job->track->count; ++i) {
        dl_peer *peer = &job->track->peers[i];
        if (!covers(peer, start, end)) continue;
        if (first == job->track->count) first = i;
        if (!peer->active) {
            first = i;
            break;
        }
    }
    if (first < job->track->count) ++job->track->peers[first].active;
    pthread_mutex_unlock(&job->mutex);
    return first;
}

static int try_peer(const worker_args *worker, dl_peer *peer, uint64_t start,
                    size_t length, void *buffer) {
    download_job *job = worker->job;
    fprintf(stderr, "[Downloader worker %u] %s:%u timestamp=%" PRIu64
            " offset=%" PRIu64 " length=%zu\n", worker->number, peer->host,
            (unsigned)peer->port, peer->timestamp, start, length);
    const char *error = fetch_chunk(peer->host, peer->port, job->track->name,
                                    start, length, buffer, 1);
    pthread_mutex_lock(&job->mutex);
    --peer->active;
    pthread_mutex_unlock(&job->mutex);
    if (!error) return 0;
    fprintf(stderr, "[Downloader worker %u] %s:%u offset=%" PRIu64
            " %s; checking fallback peers\n", worker->number, peer->host,
            (unsigned)peer->port, start, error);
    return -1;
}

static int fetch_range(const worker_args *worker, uint64_t start, uint64_t end,
                       unsigned char *buffer, uint64_t *shorter_end) {
    download_job *job = worker->job;
    track_file *track = job->track;
    size_t length = (size_t)(end - start);
    size_t first = reserve_peer(job, start, end);
    if (first < track->count && !try_peer(worker, &track->peers[first], start, length, buffer))
        return 0;
    *shorter_end = start;
    for (size_t i = 0; i < track->count; ++i) {
        dl_peer *peer = &track->peers[i];
        if (peer->start > start) continue;
        if (peer->end < end) {
            if (peer->end > *shorter_end) *shorter_end = peer->end;
            continue;
        }
        if (i == first) continue;
        pthread_mutex_lock(&job->mutex);
        ++peer->active;
        pthread_mutex_unlock(&job->mutex);
        if (!try_peer(worker, peer, start, length, buffer)) return 0;
    }
    return -1;
}

static int fetch_slot(const worker_args *worker, uint64_t offset, size_t length,
                      unsigned char *buffer) {
    uint64_t start = offset, limit = offset + length, end = limit;
    while (start < limit) {
        uint64_t shorter_end;
        if (!fetch_range(worker, start, end, buffer + (size_t)(start - offset), &shorter_end)) {
            start = end;
            end = limit;
        } else {
            if (shorter_end == start) return -1;
            end = shorter_end;
        }
    }
    return 0;
}

static ssize_t read_at(int fd, void *buffer, size_t length, off_t offset) {
    ssize_t result;
    do {
        result = pread(fd, buffer, length, offset);
    } while (result < 0 && errno == EINTR);
    return result;
}

/* The directory lock excludes competing downloaders. The map lock coordinates
 * publication of durable marks and resets with the separate serving process. */
static int lock_map(int fd, int mode) {
    int result;
    do {
        result = flock(fd, mode);
    } while (result && errno == EINTR);
    return result;
}

static int mark_complete(download_job *job, uint64_t mark) {
    unsigned char completed = 1;
    pthread_mutex_lock(&job->mutex);
    int result = lock_map(job->map_fd, LOCK_EX);
    if (!result) {
        result = write_at(job->map_fd, &completed, 1, mark) || fdatasync(job->map_fd);
        if (result) {
            completed = 0;
            (void)write_at(job->map_fd, &completed, 1, mark);
        }
        lock_map(job->map_fd, LOCK_UN);
    }
    pthread_mutex_unlock(&job->mutex);
    return result;
}

static void *download_worker(void *argument) {
    worker_args *worker = argument;
    download_job *job = worker->job;
    for (;;) {
        pthread_mutex_lock(&job->mutex);
        uint64_t slot = job->next < job->slots ? job->next++ : job->slots;
        pthread_mutex_unlock(&job->mutex);
        if (slot == job->slots) return NULL;

        uint64_t mark = MAP_HEADER_SIZE + slot;
        unsigned char completed;
        ssize_t marked = read_at(job->map_fd, &completed, 1, (off_t)mark);
        if (marked == 1 && completed == 1) continue;

        uint64_t offset = slot * CHUNK_LIMIT;
        uint64_t remaining = job->track->size - offset;
        size_t length = remaining < CHUNK_LIMIT ? (size_t)remaining : CHUNK_LIMIT;
        unsigned char buffer[CHUNK_LIMIT];
        const char *error = NULL;
        if (marked < 0) {
            error = "cannot read resume state";
        } else if (fetch_slot(worker, offset, length, buffer)) {
            error = "no remaining peer could serve the required bytes";
        } else if (write_at(job->part_fd, buffer, length, offset) ||
                   fdatasync(job->part_fd) ||
                   mark_complete(job, mark)) {
            error = "cannot persist downloaded chunk";
        }
        if (error) {
            fprintf(stderr, "[Downloader worker %u] offset=%" PRIu64 " length=%zu: %s\n",
                    worker->number, offset, length, error);
            pthread_mutex_lock(&job->mutex);
            ++job->failures;
            pthread_mutex_unlock(&job->mutex);
        }
    }
}

static void run_workers(download_job *job) {
    pthread_t threads[WORKERS - 1];
    worker_args args[WORKERS];
    size_t started = 0;
    for (; started < WORKERS - 1 && started + 1 < job->slots; ++started) {
        args[started] = (worker_args){job, (unsigned)started + 2};
        if (pthread_create(&threads[started], NULL, download_worker, &args[started])) {
            fprintf(stderr, "[Downloader] Could not start another worker; continuing\n");
            break;
        }
    }
    args[WORKERS - 1] = (worker_args){job, 1};
    download_worker(&args[WORKERS - 1]);
    for (size_t i = 0; i < started; ++i) pthread_join(threads[i], NULL);
}

static int open_state(int dir, const char *name, struct stat *info) {
    int fd = openat(dir, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, info) || !S_ISREG(info->st_mode) || info->st_nlink != 1) {
        close(fd);
        return -1;
    }
    return fd;
}

static int prepare_state(download_job *job, int dir) {
    struct stat part_info, map_info;
    job->part_fd = open_state(dir, "data", &part_info);
    job->map_fd = open_state(dir, "map", &map_info);
    if (job->part_fd < 0 || job->map_fd < 0 || lock_map(job->map_fd, LOCK_EX)) return -1;

    char expected[MAP_HEADER_SIZE] = {0}, stored[MAP_HEADER_SIZE];
    snprintf(expected, sizeof(expected), "DL1 %" PRIu64 " %s", job->track->size, job->track->md5);
    if ((uint64_t)part_info.st_size == job->track->size &&
        read_at(job->map_fd, stored, sizeof(stored), 0) == sizeof(stored) &&
        memcmp(stored, expected, sizeof(stored)) == 0)
        return lock_map(job->map_fd, LOCK_UN);

    /* Clear old completion marks before reusing any data from an earlier version. */
    if (ftruncate(job->map_fd, 0) || fdatasync(job->map_fd) ||
        ftruncate(job->part_fd, 0) || ftruncate(job->part_fd, (off_t)job->track->size) ||
        fdatasync(job->part_fd) || write_at(job->map_fd, expected, sizeof(expected), 0) ||
        fdatasync(job->map_fd)) return -1;
    return lock_map(job->map_fd, LOCK_UN);
}

static int content_matches(const char *path, const track_file *track) {
    struct stat info;
    char digest[PROTOCOL_MD5_HEX_LEN + 1];
    if (lstat(path, &info) || !S_ISREG(info.st_mode) ||
        (uint64_t)info.st_size != track->size || protocol_md5_file(path, digest)) return -1;
    return strcmp(digest, track->md5) == 0 ? 0 : 1;
}

static int publish_shared(const char *output, const char *shared, const track_file *track) {
    char destination[4096], temporary[4096], staging[4096];
    if (snprintf(destination, sizeof(destination), "%s/%s", shared, track->name) >=
        (int)sizeof(destination)) return -1;
    if (!link(output, destination)) return 0;
    if (errno == EEXIST) return content_matches(destination, track);
    if (errno != EXDEV ||
        snprintf(staging, sizeof(staging), "%s/.downloader", shared) >= (int)sizeof(staging) ||
        snprintf(temporary, sizeof(temporary), "%s/share-XXXXXX", staging) >= (int)sizeof(temporary) ||
        (mkdir(staging, 0700) && errno != EEXIST)) return -1;

    int source = open(output, O_RDONLY | O_NOFOLLOW);
    if (source < 0) return -1;
    int target = mkstemp(temporary);
    if (target < 0) {
        close(source);
        return -1;
    }
    unsigned char buffer[8192];
    uint64_t offset = 0;
    int result = -1;
    while (offset < track->size) {
        ssize_t amount = read_at(source, buffer, sizeof(buffer), (off_t)offset);
        if (amount <= 0 || write_at(target, buffer, (size_t)amount, offset)) goto done;
        offset += (uint64_t)amount;
    }
    if (fdatasync(target) || content_matches(temporary, track)) goto done;
    if (!link(temporary, destination)) result = 0;
    else if (errno == EEXIST) result = content_matches(destination, track);
done:
    close(target);
    close(source);
    unlink(temporary);
    return result;
}

static const char *download_track(const char *directory, const char *shared, track_file *track) {
    char output[4096], state[4096], job_path[4096], part[4096];
    if (snprintf(output, sizeof(output), "%s/%s", directory, track->name) >= (int)sizeof(output) ||
        snprintf(state, sizeof(state), "%s/.downloader", directory) >= (int)sizeof(state) ||
        snprintf(job_path, sizeof(job_path), "%s/%s", state, track->name) >= (int)sizeof(job_path) ||
        snprintf(part, sizeof(part), "%s/data", job_path) >= (int)sizeof(part))
        return "Download path is too long";

    struct stat info;
    if (shared) {
        char shared_path[4096];
        if (snprintf(shared_path, sizeof(shared_path), "%s/%s", shared, track->name) >=
            (int)sizeof(shared_path)) return "Shared path is too long";
        if (!lstat(shared_path, &info) && content_matches(shared_path, track))
            return "Shared destination already exists and does not match tracker metadata";
    }
    const char *error = "Cannot open download state directory";
    if (mkdir(state, 0700) && errno != EEXIST) return error;
    int root = open(state, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root < 0) return error;
    int dir = -1;
    download_job job = {
        .track = track, .part_fd = -1, .map_fd = -1,
        .slots = (track->size + CHUNK_LIMIT - 1) / CHUNK_LIMIT,
        .mutex = PTHREAD_MUTEX_INITIALIZER
    };
    if (mkdirat(root, track->name, 0700) && errno != EEXIST) goto done;
    dir = openat(root, track->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) goto done;
    error = "Another download owns this destination or its lock is unavailable";
    if (flock(dir, LOCK_EX | LOCK_NB)) goto done;

    if (lstat(output, &info) == 0) {
        error = "Destination already exists and does not match tracker size and MD5";
        if (content_matches(output, track)) goto done;
    } else {
        error = "Cannot prepare resumable download";
        if (errno != ENOENT || prepare_state(&job, dir)) goto done;
        fprintf(stderr, "[Downloader] Requesting %s (%" PRIu64 " bytes)\n", track->name, track->size);
        run_workers(&job);
        error = "Download incomplete; GET the tracker again to resume";
        if (job.failures) goto done;

        error = "Cannot verify downloaded file size and MD5";
        int verified = content_matches(part, track);
        if (verified < 0) goto done;
        if (verified > 0) {
            error = "Final MD5 mismatch; GET the tracker again to refetch all chunks";
            if (lock_map(job.map_fd, LOCK_EX) ||
                ftruncate(job.map_fd, 0) || fdatasync(job.map_fd))
                error = "Final MD5 mismatch and resume map could not be reset";
            goto done;
        }
        error = "Cannot publish verified file without replacing the destination";
        if (linkat(dir, "data", AT_FDCWD, output, 0)) goto done;
    }

    error = "Verified payload could not be published in the shared directory";
    if (shared && publish_shared(output, shared, track)) goto done;
    if ((unlinkat(dir, "data", 0) && errno != ENOENT) ||
        (unlinkat(dir, "map", 0) && errno != ENOENT) ||
        unlinkat(root, track->name, AT_REMOVEDIR))
        fprintf(stderr, "[Downloader] Could not remove all resume state for %s\n", track->name);
    fprintf(stderr, "[Downloader] Complete %s (MD5 verified: %s)\n", output, track->md5);
    error = NULL;
done:
    if (job.part_fd >= 0) close(job.part_fd);
    if (job.map_fd >= 0) close(job.map_fd);
    if (dir >= 0) close(dir);
    close(root);
    pthread_mutex_destroy(&job.mutex);
    return error;
}

int dl_download_track(const char *tracker_path, const char *directory,
                      char *error, size_t error_size) {
    return dl_download_shared(tracker_path, directory, NULL, error, error_size);
}

int dl_download_shared(const char *tracker_path, const char *directory,
                       const char *shared, char *error, size_t error_size) {
    if (!tracker_path || !*tracker_path || !directory || !*directory)
        return fail(error, error_size, "Invalid tracker path or destination directory");
    track_file track = {0};
    const char *message = "Cannot read valid tracker metadata";
    if (!read_track(tracker_path, &track)) message = download_track(directory, shared, &track);
    free(track.peers);
    if (message) return fail(error, error_size, message);
    if (error && error_size) error[0] = '\0';
    return 0;
}

/* Open both files relative to one job directory and pin the map generation
 * until the caller finishes inspecting marks or reading marked bytes. */
static int open_partial(const char *directory, const char *name,
                        int *map, int *data, uint64_t *size) {
    char path[4096];
    if (!track_name_valid(name) ||
        snprintf(path, sizeof(path), "%s/.downloader/%s", directory, name) >= (int)sizeof(path))
        return -1;
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dir < 0) return -1;
    *map = openat(dir, "map", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    *data = -1;
    if (*map < 0 || lock_map(*map, LOCK_SH)) goto failed;
    *data = openat(dir, "data", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat info;
    char header[MAP_HEADER_SIZE + 1] = {0}, digest[33];
    if (*data < 0 || fstat(*data, &info) || !S_ISREG(info.st_mode) ||
        read_at(*map, header, MAP_HEADER_SIZE, 0) != MAP_HEADER_SIZE ||
        sscanf(header, "DL1 %" SCNu64 " %32[0-9a-f]", size, digest) != 2 ||
        strlen(digest) != 32 || *size > INT64_MAX || *size != (uint64_t)info.st_size)
        goto failed;
    close(dir);
    return 0;
failed:
    if (*map >= 0) close(*map);
    if (*data >= 0) close(*data);
    close(dir);
    return -1;
}

int dl_read_partial(const char *directory, const char *name, uint64_t offset,
                    void *buffer, size_t length) {
    int map, data, result = -1;
    uint64_t size;
    if (!length || length > CHUNK_LIMIT || open_partial(directory, name, &map, &data, &size))
        return -1;
    if (offset >= size || length > size - offset) goto done;
    for (uint64_t slot = offset / CHUNK_LIMIT; slot <= (offset + length - 1) / CHUNK_LIMIT; ++slot) {
        unsigned char mark;
        if (read_at(map, &mark, 1, (off_t)(MAP_HEADER_SIZE + slot)) != 1 || mark != 1)
            goto done;
    }
    if (read_at(data, buffer, length, (off_t)offset) == (ssize_t)length) result = 0;
done:
    close(data);
    close(map);
    return result;
}

int dl_partial_ranges(const char *directory, const char *name, dl_range **ranges, size_t *count) {
    *ranges = NULL;
    *count = 0;
    int map, data;
    uint64_t size;
    if (open_partial(directory, name, &map, &data, &size)) return -1;
    uint64_t slots = (size + CHUNK_LIMIT - 1) / CHUNK_LIMIT, slot = 0;
    size_t capacity = 0;
    int result = -1;
    unsigned char marks[4096];
    while (slot < slots) {
        size_t wanted = slots - slot < sizeof(marks) ? (size_t)(slots - slot) : sizeof(marks);
        ssize_t amount = read_at(map, marks, wanted, (off_t)(MAP_HEADER_SIZE + slot));
        if (amount < 0) goto done;
        if (!amount) break;
        for (ssize_t i = 0; i < amount; ++i, ++slot) {
            if (marks[i] != 1) continue;
            uint64_t first = slot * CHUNK_LIMIT;
            uint64_t last = first + CHUNK_LIMIT <= size ? first + CHUNK_LIMIT - 1 : size - 1;
            if (*count && (*ranges)[*count - 1].last + 1 == first) {
                (*ranges)[*count - 1].last = last;
                continue;
            }
            if (*count == capacity) {
                if (capacity > SIZE_MAX / (2 * sizeof(**ranges))) goto done;
                capacity = capacity ? 2 * capacity : 8;
                dl_range *grown = realloc(*ranges, capacity * sizeof(**ranges));
                if (!grown) goto done;
                *ranges = grown;
            }
            (*ranges)[(*count)++] = (dl_range){first, last};
        }
    }
    result = 0;
done:
    close(data);
    close(map);
    if (result) {
        free(*ranges);
        *ranges = NULL;
        *count = 0;
    }
    return result;
}

static uint64_t source_hash(const char *text) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (; *text; ++text) hash = (hash ^ (unsigned char)*text) * UINT64_C(1099511628211);
    return hash;
}

int dl_source_signature(const char *tracker_path, const char *own_ip,
                        uint16_t own_port, uint64_t *signature) {
    track_file track = {0};
    int result = read_track(tracker_path, &track);
    if (!result) {
        *signature = source_hash(track.md5) + track.size;
        /* Addition makes row order irrelevant. Timestamp-only refreshes do
         * not revive exhausted attempts, but changed endpoints or ranges do. */
        for (size_t i = 0; i < track.count; ++i) {
            const dl_peer *p = &track.peers[i];
            if (p->port == own_port && !strcmp(p->host, own_ip)) continue;
            char entry[96];
            snprintf(entry, sizeof(entry), "%s:%u:%" PRIu64 ":%" PRIu64,
                     p->host, (unsigned)p->port, p->start, p->end);
            *signature += source_hash(entry);
        }
    }
    free(track.peers);
    return result;
}
