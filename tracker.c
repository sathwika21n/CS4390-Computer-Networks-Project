#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

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
            /* TODO: replace placeholder with real .track file list */
            const char *reply = "<REP LIST 0>\n<REP LIST END>\n";
            send_all(client, reply, strlen(reply));
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