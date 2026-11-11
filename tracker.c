#define _POSIX_C_SOURCE 200809L

#include "protocol.h"
#include "utils.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS 100
#define TRACKER_TIMEOUT_SECONDS 900 /* Peer refresh defaults to 15 minutes. */

static unsigned short tracker_port;
static char shared_dir[256];
static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t thread_cond = PTHREAD_COND_INITIALIZER;
static int active_threads = 0;

static void read_config(void) {
    FILE *f = fopen("sconfig", "r");
    unsigned int p;
    if (!f) { perror("sconfig"); exit(EXIT_FAILURE); }
    if (fscanf(f, "%u", &p) != 1 || p == 0 || p > 65535 ||
        fscanf(f, "%255s", shared_dir) != 1) {
        fprintf(stderr, "Invalid sconfig (expected port and tracker directory)\n");
        fclose(f);
        exit(EXIT_FAILURE);
    }
    fclose(f);
    tracker_port = (unsigned short)p;
    struct stat st;
    if (stat(shared_dir, &st) != 0) {
        if (errno != ENOENT || mkdir(shared_dir, 0700) != 0) {
            perror("mkdir tracker directory");
            exit(EXIT_FAILURE);
        }
    } else if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%s is not a directory\n", shared_dir);
        exit(EXIT_FAILURE);
    }
}

/* Never allow requests to turn a filename into a path outside the tracker directory. */
static int valid_name(const char *s) {
    if (!s || !*s || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    for (; *s; ++s) {
        if (*s == '/' || *s == '\\' || isspace((unsigned char)*s) ||
            *s == '<' || *s == '>' || *s == ':') return 0;
    }
    return 1;
}

static int tracker_path(char *path, size_t cap, const char *filename) {
    if (!valid_name(filename)) return -1;
    int n = snprintf(path, cap, "%s/%s.track", shared_dir, filename);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int ends_with_track(const char *filename) {
    size_t len = strlen(filename);
    return len > 6 && strcmp(filename + len - 6, ".track") == 0;
}

static int is_peer_entry(const char *line, char *ip, size_t ip_cap,
                         unsigned int *port, long long *stamp) {
    unsigned long long start, end;
    char parsed_ip[64], extra;
    if (sscanf(line, "%63[^:]:%u:%llu:%llu:%lld %c", parsed_ip,
               port, &start, &end, stamp, &extra) != 5) return 0;
    if (strlen(parsed_ip) >= ip_cap) return 0;
    strcpy(ip, parsed_ip);
    return 1;
}

static int read_track_header(const char *path, char *name, long long *size, char *md5) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "Filename: %255s", name) == 1 && strncmp(line, "Filename:", 9) == 0) found |= 1;
        else if (sscanf(line, "Filesize: %lld", size) == 1 && strncmp(line, "Filesize:", 9) == 0) found |= 2;
        else if (sscanf(line, "MD5: %32s", md5) == 1 && strncmp(line, "MD5:", 4) == 0) found |= 4;
    }
    fclose(f);
    return found == 7;
}

static void reply_create(int fd, const char *status) {
    char reply[64];
    snprintf(reply, sizeof(reply), "<createtracker %s>", status);
    (void)protocol_send_line(fd, reply);
}

static void reply_update(int fd, const char *name, const char *status) {
    char reply[512];
    snprintf(reply, sizeof(reply), "<updatetracker %s %s>", name, status);
    (void)protocol_send_line(fd, reply);
}

static void handle_create(int fd, const protocol_request_t *req) {
    char path[600];
    if (tracker_path(path, sizeof(path), req->filename) != 0) {
        reply_create(fd, "fail");
        return;
    }
    pthread_mutex_lock(&file_lock);
    /* Exclusive create prevents duplicate tracker entries, including concurrent calls. */
    FILE *f = fopen(path, "wx");
    if (!f) {
        const char *status = errno == EEXIST ? "ferr" : "fail";
        pthread_mutex_unlock(&file_lock);
        reply_create(fd, status);
        return;
    }
    char desc[PROTOCOL_NAME_MAX];
    snprintf(desc, sizeof(desc), "%s", req->description);
    for (char *p = desc; *p; ++p) if (*p == '_') *p = ' ';
    int ok = fprintf(f, "Filename: %s\nFilesize: %llu\nDescription: %s\nMD5: %s\n"
                        "#list of peers follows next\n%s:%u:0:%llu:%lld\n",
                     req->filename, (unsigned long long)req->filesize,
                     desc, req->md5, req->ip_address, (unsigned)req->port,
                     (unsigned long long)req->filesize, (long long)time(NULL)) >= 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) unlink(path);
    pthread_mutex_unlock(&file_lock);
    reply_create(fd, ok ? "succ" : "fail");
}

/* Replace one peer's entry, add new peers, and drop entries whose refresh expired.
   A temporary file and rename prevent readers from seeing a half-written tracker. */
static void handle_update(int fd, const protocol_request_t *req) {
    char path[600];
    if (tracker_path(path, sizeof(path), req->filename) != 0) {
        reply_update(fd, req->filename, "fail");
        return;
    }
    pthread_mutex_lock(&file_lock);
    FILE *src = fopen(path, "r");
    if (!src) {
        const char *status = errno == ENOENT ? "ferr" : "fail";
        pthread_mutex_unlock(&file_lock);
        reply_update(fd, req->filename, status);
        return;
    }
    char temp[650];
    snprintf(temp, sizeof(temp), "%s/.updateXXXXXX", shared_dir);
    int tmpfd = mkstemp(temp);
    if (tmpfd < 0) {
        fclose(src);
        pthread_mutex_unlock(&file_lock);
        reply_update(fd, req->filename, "fail");
        return;
    }
    FILE *dst = fdopen(tmpfd, "w");
    if (!dst) {
        close(tmpfd); unlink(temp); fclose(src);
        pthread_mutex_unlock(&file_lock);
        reply_update(fd, req->filename, "fail");
        return;
    }
    char line[2048];
    int ok = 1;
    time_t now = time(NULL);
    while (fgets(line, sizeof(line), src)) {
        char ip[64];
        unsigned int p = 0;
        long long timestamp = 0;
        if (is_peer_entry(line, ip, sizeof(ip), &p, &timestamp)) {
            /* The requesting peer gets a fresh entry below. */
            if (strcmp(ip, req->ip_address) == 0 && p == req->port) continue;
            if (timestamp <= 0 || (long long)now - timestamp > TRACKER_TIMEOUT_SECONDS) continue;
        }
        if (fputs(line, dst) == EOF) { ok = 0; break; }
    }
    if (ferror(src)) ok = 0;
    if (ok && fprintf(dst, "%s:%u:%llu:%llu:%lld\n", req->ip_address,
                      (unsigned)req->port,
                      (unsigned long long)req->start_bytes,
                      (unsigned long long)req->end_bytes,
                      (long long)now) < 0) ok = 0;
    if (fclose(src) != 0) ok = 0;
    if (fclose(dst) != 0) ok = 0;
    if (ok && rename(temp, path) != 0) ok = 0;
    if (!ok) unlink(temp);
    pthread_mutex_unlock(&file_lock);
    reply_update(fd, req->filename, ok ? "succ" : "fail");
}

static void handle_list(int fd) {
    pthread_mutex_lock(&file_lock);
    DIR *dir = opendir(shared_dir);
    if (!dir) {
        pthread_mutex_unlock(&file_lock);
        (void)protocol_send_line(fd, "<REP LIST 0>");
        (void)protocol_send_line(fd, "<REP LIST END>");
        return;
    }
    /* Store result rows so LIST count always equals the number of rows sent. */
    char **rows = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!ends_with_track(entry->d_name)) continue;
        char path[600], name[256] = "", md5[33] = "";
        long long size = 0;
        int pn = snprintf(path, sizeof(path), "%s/%s", shared_dir, entry->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(path) ||
            !read_track_header(path, name, &size, md5)) continue;
        char row[512];
        snprintf(row, sizeof(row), "<%zu %s %lld %s>", count + 1, name, size, md5);
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            char **grown = realloc(rows, next * sizeof(*rows));
            if (!grown) break;
            rows = grown;
            capacity = next;
        }
        rows[count] = strdup(row);
        if (!rows[count]) break;
        ++count;
    }
    closedir(dir);
    char header[64];
    snprintf(header, sizeof(header), "<REP LIST %zu>", count);
    (void)protocol_send_line(fd, header);
    for (size_t i = 0; i < count; ++i) {
        (void)protocol_send_line(fd, rows[i]);
        free(rows[i]);
    }
    free(rows);
    (void)protocol_send_line(fd, "<REP LIST END>");
    pthread_mutex_unlock(&file_lock);
}

/* GET retrieves the .track metadata, not the peer's shared file itself. */
static void handle_get(int fd, const protocol_request_t *req) {
    if (!valid_name(req->filename) || !ends_with_track(req->filename)) return;
    char path[600];
    int pn = snprintf(path, sizeof(path), "%s/%s", shared_dir, req->filename);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return;
    pthread_mutex_lock(&file_lock);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "GET: missing tracker %s\n", req->filename);
        pthread_mutex_unlock(&file_lock);
        return; /* Project doesnt specify a GET error response. */
    }
    char md5[33];
    if (calculateMD5(path, md5) != 0) {
        fclose(f);
        pthread_mutex_unlock(&file_lock);
        return;
    }
    (void)protocol_send_line(fd, "<REP GET BEGIN>");
    /* Send tracker content unchanged; the client reconstructs it and verifies MD5. */
    char bytes[4096];
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        size_t offset = 0;
        while (offset < n) {
            ssize_t sent = send(fd, bytes + offset, n - offset, 0);
            if (sent <= 0) break;
            offset += (size_t)sent;
        }
        if (offset != n) break;
    }
    fclose(f);
    /* One framing newline is required by the peer_client GET reader. */
    (void)send(fd, "\n", 1, 0);
    char footer[80];
    snprintf(footer, sizeof(footer), "<REP GET END %s>", md5);
    (void)protocol_send_line(fd, footer);
    pthread_mutex_unlock(&file_lock);
}

static void *handle_client(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    char line[PROTOCOL_LINE_MAX];
    int status = protocol_read_line(fd, line, sizeof(line));
    if (status == 1) {
        protocol_request_t req;
        printf("Received: %s\n", line);
        fflush(stdout);
        if (protocol_parse_request(line, &req) == 0) {
            switch (req.kind) {
                case PROTOCOL_REQUEST_CREATE_TRACKER: handle_create(fd, &req); break;
                case PROTOCOL_REQUEST_UPDATE_TRACKER: handle_update(fd, &req); break;
                case PROTOCOL_REQUEST_LIST: handle_list(fd); break;
                case PROTOCOL_REQUEST_GET: handle_get(fd, &req); break;
                default: break;
            }
        } else {
            fprintf(stderr, "Invalid tracker request: %s\n", line);
            /* the spec says a bad createtracker/updatetracker still gets a fail reply */
            if (strncmp(line, "<createtracker", 14) == 0) {
                reply_create(fd, "fail");
            } else if (strncmp(line, "<updatetracker", 14) == 0) {
                char name[256] = "";
                if (sscanf(line, "<updatetracker %255[^ >]", name) == 1) {
                    reply_update(fd, name, "fail");
                } else {
                    fprintf(stderr, "Invalid updatetracker: missing filename\n");
                }
            }
        }
    }
    close(fd);
    pthread_mutex_lock(&thread_lock);
    --active_threads;
    pthread_cond_signal(&thread_cond);
    pthread_mutex_unlock(&thread_lock);
    return NULL;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    read_config();
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) { perror("socket"); return EXIT_FAILURE; }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(tracker_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 10) != 0) {
        perror("bind/listen");
        close(listener);
        return EXIT_FAILURE;
    }
    printf("Tracker listening on port %u (dir: %s)\n", tracker_port, shared_dir);
    fflush(stdout);
    for (;;) {
        pthread_mutex_lock(&thread_lock);
        while (active_threads >= MAX_THREADS)
            pthread_cond_wait(&thread_cond, &thread_lock);
        pthread_mutex_unlock(&thread_lock);
        int *client = malloc(sizeof(*client));
        if (!client) { perror("malloc"); continue; }
        *client = accept(listener, NULL, NULL);
        if (*client < 0) { perror("accept"); free(client); continue; }
        pthread_mutex_lock(&thread_lock);
        ++active_threads;
        pthread_mutex_unlock(&thread_lock);
        pthread_t thread;
        if (pthread_create(&thread, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            close(*client);
            free(client);
            pthread_mutex_lock(&thread_lock);
            --active_threads;
            pthread_cond_signal(&thread_cond);
            pthread_mutex_unlock(&thread_lock);
        } else {
            pthread_detach(thread);
        }
    }
}
