#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <dirent.h>
#include <time.h>

#define BUF_SIZE    1024
#define MAX_THREADS 100

int  port;
char shared_dir[256];

static pthread_mutex_t thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  thread_cond = PTHREAD_COND_INITIALIZER;
static int active_threads = 0;

/* sconfig format: line 1 = port, line 2 = shared folder */
void read_config(void) {
    FILE *f = fopen("sconfig", "r");
    if (f == NULL) { perror("sconfig"); exit(1); }
    if (fscanf(f, "%d", &port) != 1) {
        fprintf(stderr, "bad config: missing port\n"); exit(1);
    }
    if (port < 1 || port > 65535) {
        fprintf(stderr, "bad config: invalid port\n"); exit(1);
    }
    if (fscanf(f, "%255s", shared_dir) != 1) {
        fprintf(stderr, "bad config: missing shared_dir\n"); exit(1);
    }
    fclose(f);
}

/* send() may send only part of the buffer, so loop until done */
static void send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t r = send(fd, buf + sent, len - sent, 0);
        if (r <= 0) break;
        sent += (size_t)r;
    }
}

/* Reads Filename, Filesize, and MD5 from one .track file; returns 1 if all were found */
static int read_track_header(const char *path, char *name, long *size, char *md5) {
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "Filename: %255s", name) == 1) found |= 1;
        else if (sscanf(line, "Filesize: %ld", size) == 1) found |= 2;
        else if (sscanf(line, "MD5: %32s", md5) == 1) found |= 4;
    }
    fclose(f);
    return found == 7;
}

static void handle_createtracker(int client, const char *msg) {
    char name[256], desc[256], md5[33], ip[64];
    long size;
    int peer_port;
    const char *fail = "<createtracker fail>\n";
    const char *ferr = "<createtracker ferr>\n";
    const char *succ = "<createtracker succ>\n";

    if (sscanf(msg, "<createtracker %255s %ld %255s %32s %63s %d>",
               name, &size, desc, md5, ip, &peer_port) != 6
        || strchr(name, '/') != NULL || size < 0) {
        send_all(client, fail, strlen(fail));
        return;
    }

    /* peers send underscores for spaces; store spaces like the spec example */
    for (char *c = desc; *c; c++) if (*c == '_') *c = ' ';

    char path[600];
    snprintf(path, sizeof(path), "%s/%s.track", shared_dir, name);

    /* "wx" fails if the file already exists, so two peers can't both create it */
    FILE *f = fopen(path, "wx");
    if (f == NULL) {
        FILE *exists = fopen(path, "r");
        if (exists) { fclose(exists); send_all(client, ferr, strlen(ferr)); }
        else send_all(client, fail, strlen(fail));
        return;
    }

    fprintf(f, "Filename: %s\n", name);
    fprintf(f, "Filesize: %ld\n", size);
    fprintf(f, "Description: %s\n", desc);
    fprintf(f, "MD5: %s\n", md5);
    fprintf(f, "#list of peers follows next\n");
    fprintf(f, "%s:%d:0:%ld:%ld\n", ip, peer_port, size, (long)time(NULL));
    fclose(f);

    send_all(client, succ, strlen(succ));
}

static void handle_list(int client) {
    char entries[8192] = "";
    int count = 0;

    DIR *dir = opendir(shared_dir);
    if (dir != NULL) {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len < 7 || strcmp(e->d_name + len - 6, ".track") != 0) continue;

            char path[600], name[256], md5[33];
            long size;
            snprintf(path, sizeof(path), "%s/%s", shared_dir, e->d_name);
            if (!read_track_header(path, name, &size, md5)) continue;

            char row[400];
            snprintf(row, sizeof(row), "<%d %s %ld %s>\n", ++count, name, size, md5);
            strncat(entries, row, sizeof(entries) - strlen(entries) - 1);
        }
        closedir(dir);
    }

    char header[64];
    snprintf(header, sizeof(header), "<REP LIST %d>\n", count);
    send_all(client, header, strlen(header));
    send_all(client, entries, strlen(entries));
    send_all(client, "<REP LIST END>\n", 15);
}

void *handle_client(void *arg) {
    int client = *(int *)arg;
    free(arg);

    char buf[BUF_SIZE];
    memset(buf, 0, BUF_SIZE);

    /* TCP may split one message, so read until newline */
    int n = 0;
    while (n < BUF_SIZE - 1) {
        int r = recv(client, buf + n, BUF_SIZE - 1 - n, 0);
        if (r < 0) { perror("recv"); break; }
        if (r == 0) break;
        n += r;
        if (buf[n - 1] == '\n') break;
    }

    if (n > 0) {
        printf("Received: %s", buf);
        if (strncmp(buf, "<REQ LIST>", 10) == 0) {
            handle_list(client);
        } else if (strncmp(buf, "<createtracker ", 15) == 0) {
            handle_createtracker(client, buf);
        }
    }

    close(client);

    pthread_mutex_lock(&thread_lock);
    active_threads--;
    pthread_cond_signal(&thread_cond);
    pthread_mutex_unlock(&thread_lock);

    return NULL;
}

int main(void) {
    /* prevent a peer disconnecting mid-send from killing the tracker */
    signal(SIGPIPE, SIG_IGN);
    read_config();

    int sockid = socket(AF_INET, SOCK_STREAM, 0);
    if (sockid < 0) { perror("socket"); exit(1); }

    int opt = 1;
    setsockopt(sockid, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);

    if (bind(sockid, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(sockid, 10) < 0) {
        perror("listen"); exit(1);
    }
    printf("Tracker listening on port %d\n", port);

    while (1) {
        /* wait if MAX_THREADS peers are already connected */
        pthread_mutex_lock(&thread_lock);
        while (active_threads >= MAX_THREADS)
            pthread_cond_wait(&thread_cond, &thread_lock);
        pthread_mutex_unlock(&thread_lock);

        /* each thread gets its own copy of the socket */
        int *client = malloc(sizeof(int));
        if (!client) { perror("malloc"); continue; }

        *client = accept(sockid, NULL, NULL);
        if (*client < 0) {
            perror("accept"); free(client); continue;
        }

        pthread_mutex_lock(&thread_lock);
        active_threads++;
        pthread_mutex_unlock(&thread_lock);

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, client) != 0) {
            perror("pthread_create");
            close(*client); free(client);
            pthread_mutex_lock(&thread_lock);
            active_threads--;
            pthread_mutex_unlock(&thread_lock);
            continue;
        }
        pthread_detach(tid);
    }
}
