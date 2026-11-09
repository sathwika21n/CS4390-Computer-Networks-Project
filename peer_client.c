#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char tracker_host[256];
    unsigned short tracker_port;
    char peer_ip[64];
    unsigned short peer_port;
    char shared_dir[512];
    char download_dir[512];
    unsigned int update_interval;
} client_config_t;

static client_config_t config;
static pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shutdown_condition = PTHREAD_COND_INITIALIZER;
static int shutting_down;

static char *trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {
        text++;
    }
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

static int parse_port(const char *value, unsigned short *port)
{
    char *end = NULL;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535) {
        return -1;
    }
    *port = (unsigned short)parsed;
    return 0;
}

static int load_config(const char *path)
{
    FILE *file = fopen(path, "r");
    char line[1024];
    unsigned int seen = 0;

    if (file == NULL) {
        perror(path);
        return -1;
    }
    memset(&config, 0, sizeof(config));
    config.update_interval = 60;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *key = trim(line);
        char *separator;
        char *value;
        char *comment = strchr(key, '#');
        if (comment != NULL) {
            *comment = '\0';
            key = trim(key);
        }
        if (*key == '\0') {
            continue;
        }
        separator = strchr(key, '=');
        if (separator == NULL) {
            fprintf(stderr, "Invalid config line: %s\n", key);
            fclose(file);
            return -1;
        }
        *separator = '\0';
        value = trim(separator + 1);
        key = trim(key);

        if (strcmp(key, "tracker_host") == 0) {
            snprintf(config.tracker_host, sizeof(config.tracker_host), "%s", value);
            seen |= 1U;
        } else if (strcmp(key, "tracker_port") == 0) {
            if (parse_port(value, &config.tracker_port) != 0) {
                fprintf(stderr, "Invalid tracker_port in %s\n", path);
                fclose(file);
                return -1;
            }
            seen |= 2U;
        } else if (strcmp(key, "peer_ip") == 0) {
            snprintf(config.peer_ip, sizeof(config.peer_ip), "%s", value);
            seen |= 4U;
        } else if (strcmp(key, "peer_port") == 0) {
            if (parse_port(value, &config.peer_port) != 0) {
                fprintf(stderr, "Invalid peer_port in %s\n", path);
                fclose(file);
                return -1;
            }
            seen |= 8U;
        } else if (strcmp(key, "shared_dir") == 0) {
            snprintf(config.shared_dir, sizeof(config.shared_dir), "%s", value);
            seen |= 16U;
        } else if (strcmp(key, "download_dir") == 0) {
            snprintf(config.download_dir, sizeof(config.download_dir), "%s", value);
            seen |= 32U;
        } else if (strcmp(key, "update_interval") == 0) {
            char *end = NULL;
            unsigned long interval = strtoul(value, &end, 10);
            if (end == value || *end != '\0' || interval == 0 || interval > 86400) {
                fprintf(stderr, "Invalid update_interval in %s (use 1-86400)\n", path);
                fclose(file);
                return -1;
            }
            config.update_interval = (unsigned int)interval;
        } else {
            fprintf(stderr, "Unknown config key: %s\n", key);
            fclose(file);
            return -1;
        }
    }
    fclose(file);

    if ((seen & 63U) != 63U) {
        fprintf(stderr, "Config must define tracker_host, tracker_port, peer_ip, peer_port, shared_dir, and download_dir.\n");
        return -1;
    }
    return 0;
}

static int ensure_directory(const char *path)
{
    struct stat status;
    if (stat(path, &status) == 0) {
        if (S_ISDIR(status.st_mode)) {
            return 0;
        }
        errno = ENOTDIR;
        return -1;
    }
    if (errno != ENOENT || mkdir(path, 0775) != 0) {
        return -1;
    }
    return 0;
}

static int connect_tracker(void)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char service[8];
    int socket_fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%u", config.tracker_port);
    if (getaddrinfo(config.tracker_host, service, &hints, &addresses) != 0) {
        fprintf(stderr, "Unable to resolve tracker host %s\n", config.tracker_host);
        return -1;
    }
    for (address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) {
            continue;
        }
        if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) {
        fprintf(stderr, "Could not connect to tracker %s:%u: %s\n",
                config.tracker_host, config.tracker_port, strerror(errno));
    }
    return socket_fd;
}

static int safe_filename(const char *filename)
{
    const unsigned char *character = (const unsigned char *)filename;
    if (filename == NULL || *filename == '\0' || strcmp(filename, ".") == 0 ||
        strcmp(filename, "..") == 0 || strlen(filename) >= PROTOCOL_NAME_MAX ||
        strchr(filename, '/') != NULL || strchr(filename, '\\') != NULL) {
        return 0;
    }
    while (*character != '\0') {
        if (isspace(*character) || *character == '<' || *character == '>') {
            return 0;
        }
        character++;
    }
    return 1;
}

static int file_metadata(const char *filename, uint64_t *filesize,
                         char md5[PROTOCOL_MD5_HEX_LEN + 1], char *path, size_t path_size)
{
    struct stat status;
    if (!safe_filename(filename) ||
        snprintf(path, path_size, "%s/%s", config.shared_dir, filename) >= (int)path_size ||
        stat(path, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0) {
        fprintf(stderr, "File must be a regular file in %s and have a name without spaces.\n",
                config.shared_dir);
        return -1;
    }
    *filesize = (uint64_t)status.st_size;
    if (md5 != NULL && protocol_md5_file(path, md5) != 0) {
        perror("Unable to calculate file MD5");
        return -1;
    }
    return 0;
}

static int update_one_file(const char *filename, int quiet)
{
    char path[1024];
    char request[PROTOCOL_LINE_MAX];
    char response_line[PROTOCOL_LINE_MAX];
    protocol_response_t response;
    uint64_t filesize;
    int socket_fd;
    int read_result;

    if (file_metadata(filename, &filesize, NULL, path, sizeof(path)) != 0) {
        return -1;
    }
    snprintf(request, sizeof(request), "<updatetracker %s 0 %llu %s %u>\n",
             filename, (unsigned long long)(filesize == 0 ? 0 : filesize - 1),
             config.peer_ip, config.peer_port);
    socket_fd = connect_tracker();
    if (socket_fd < 0) {
        return -1;
    }
    if (protocol_send_line(socket_fd, request) != 0) {
        perror("Sending updatetracker");
        close(socket_fd);
        return -1;
    }
    read_result = protocol_read_line(socket_fd, response_line, sizeof(response_line));
    close(socket_fd);
    if (read_result != 1 || protocol_parse_response(response_line, &response) != 0 ||
        response.kind != PROTOCOL_RESPONSE_UPDATE_TRACKER) {
        fprintf(stderr, "Invalid updatetracker response: %s\n",
                read_result == 1 ? response_line : "connection closed");
        return -1;
    }
    if (!quiet) {
        printf("updatetracker %s: %s\n", filename, response.status);
    }
    return strcmp(response.status, "succ") == 0 ? 0 : -1;
}

static int read_prompt(const char *prompt, char *buffer, size_t size)
{
    printf("%s", prompt);
    fflush(stdout);
    if (fgets(buffer, (int)size, stdin) == NULL) {
        return -1;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
    return 0;
}

static void command_create_tracker(void)
{
    char filename[PROTOCOL_NAME_MAX];
    char description[256];
    char path[1024];
    char md5[PROTOCOL_MD5_HEX_LEN + 1];
    char request[PROTOCOL_LINE_MAX];
    char response_line[PROTOCOL_LINE_MAX];
    protocol_response_t response;
    uint64_t filesize;
    int socket_fd;

    if (read_prompt("Shared filename (no spaces): ", filename, sizeof(filename)) != 0 ||
        read_prompt("Description (one word): ", description, sizeof(description)) != 0) {
        return;
    }
    if (!safe_filename(filename) || description[0] == '\0' || strpbrk(description, " \t<>\r\n") != NULL) {
        fprintf(stderr, "Filename and description must each be a single protocol token.\n");
        return;
    }
    if (file_metadata(filename, &filesize, md5, path, sizeof(path)) != 0) {
        return;
    }
    snprintf(request, sizeof(request), "<createtracker %s %llu %s %s %s %u>\n",
             filename, (unsigned long long)filesize, description, md5,
             config.peer_ip, config.peer_port);
    socket_fd = connect_tracker();
    if (socket_fd < 0) {
        return;
    }
    if (protocol_send_line(socket_fd, request) != 0 ||
        protocol_read_line(socket_fd, response_line, sizeof(response_line)) != 1) {
        perror("createtracker exchange");
        close(socket_fd);
        return;
    }
    close(socket_fd);
    if (protocol_parse_response(response_line, &response) != 0 ||
        response.kind != PROTOCOL_RESPONSE_CREATE_TRACKER) {
        fprintf(stderr, "Invalid createtracker response: %s\n", response_line);
        return;
    }
    printf("createtracker: %s\n", response.status);
    if (strcmp(response.status, "succ") == 0) {
        (void)update_one_file(filename, 0);
    }
}

static void command_update_tracker(void)
{
    char filename[PROTOCOL_NAME_MAX];
    if (read_prompt("Shared filename to publish/update (no spaces): ", filename,
                    sizeof(filename)) == 0) {
        (void)update_one_file(filename, 0);
    }
}

static void command_list(void)
{
    char line[PROTOCOL_LINE_MAX];
    protocol_response_t response;
    uint64_t item_count;
    uint64_t index;
    int socket_fd = connect_tracker();

    if (socket_fd < 0) {
        return;
    }
    if (protocol_send_line(socket_fd, "<REQ LIST>\n") != 0 ||
        protocol_read_line(socket_fd, line, sizeof(line)) != 1 ||
        protocol_parse_response(line, &response) != 0 ||
        response.kind != PROTOCOL_RESPONSE_LIST_HEADER) {
        fprintf(stderr, "Invalid LIST response header.\n");
        close(socket_fd);
        return;
    }
    item_count = response.count;
    printf("Tracker files (%llu):\n", (unsigned long long)item_count);
    for (index = 0; index < item_count; index++) {
        if (protocol_read_line(socket_fd, line, sizeof(line)) != 1 ||
            protocol_parse_response(line, &response) != 0 ||
            response.kind != PROTOCOL_RESPONSE_LIST_ITEM) {
            fprintf(stderr, "Invalid or incomplete LIST entry.\n");
            close(socket_fd);
            return;
        }
        printf("%llu %s %llu %s\n", (unsigned long long)response.index,
               response.filename, (unsigned long long)response.filesize, response.md5);
    }
    if (protocol_read_line(socket_fd, line, sizeof(line)) != 1 ||
        protocol_parse_response(line, &response) != 0 ||
        response.kind != PROTOCOL_RESPONSE_LIST_END) {
        fprintf(stderr, "Missing REP LIST END.\n");
    }
    close(socket_fd);
}

static int ensure_md5_casefold_equal(const char *left, const char *right)
{
    return strcasecmp(left, right) == 0;
}

static void command_get(void)
{
    char filename[PROTOCOL_NAME_MAX];
    char request[PROTOCOL_LINE_MAX];
    char line[PROTOCOL_LINE_MAX];
    char output_path[1024];
    char temporary_path[1100];
    char local_md5[PROTOCOL_MD5_HEX_LEN + 1];
    protocol_response_t response;
    FILE *output = NULL;
    int socket_fd = -1;
    int completed = 0;

    if (read_prompt("Tracker filename to GET (for example file.track): ", filename,
                    sizeof(filename)) != 0 || !safe_filename(filename)) {
        fprintf(stderr, "Invalid filename.\n");
        return;
    }
    if (snprintf(output_path, sizeof(output_path), "%s/%s", config.download_dir,
                 filename) >= (int)sizeof(output_path) ||
        snprintf(temporary_path, sizeof(temporary_path), "%s.part.%ld", output_path,
                 (long)getpid()) >= (int)sizeof(temporary_path)) {
        fprintf(stderr, "Download path is too long.\n");
        return;
    }
    snprintf(request, sizeof(request), "<GET %s >\n", filename);
    socket_fd = connect_tracker();
    if (socket_fd < 0) {
        return;
    }
    if (protocol_send_line(socket_fd, request) != 0 ||
        protocol_read_line(socket_fd, line, sizeof(line)) != 1 ||
        protocol_parse_response(line, &response) != 0 ||
        response.kind != PROTOCOL_RESPONSE_GET_BEGIN) {
        fprintf(stderr, "Invalid GET response.\n");
        goto cleanup;
    }
    output = fopen(temporary_path, "wb");
    if (output == NULL) {
        perror(temporary_path);
        goto cleanup;
    }
    for (;;) {
        int read_result = protocol_read_line(socket_fd, line, sizeof(line));
        if (read_result != 1) {
            fprintf(stderr, "Connection ended before REP GET END.\n");
            goto cleanup;
        }
        if (protocol_parse_response(line, &response) == 0 &&
            response.kind == PROTOCOL_RESPONSE_GET_END) {
            break;
        }
        if (fputs(line, output) == EOF || fputc('\n', output) == EOF) {
            perror("Writing tracker file");
            goto cleanup;
        }
    }
    /* The newline immediately before REP GET END is protocol framing, not file data. */
    {
        off_t content_length = ftello(output);
        if (content_length < 1 || fflush(output) != 0 ||
            ftruncate(fileno(output), content_length - 1) != 0) {
            perror("Trimming GET framing newline");
            goto cleanup;
        }
    }
    if (fclose(output) != 0) {
        output = NULL;
        perror("Closing downloaded tracker file");
        goto cleanup;
    }
    output = NULL;
    if (protocol_md5_file(temporary_path, local_md5) != 0) {
        perror("Calculating downloaded tracker MD5");
        goto cleanup;
    }
    if (!ensure_md5_casefold_equal(local_md5, response.md5)) {
        fprintf(stderr, "MD5 mismatch: server=%s local=%s\n", response.md5, local_md5);
        goto cleanup;
    }
    if (rename(temporary_path, output_path) != 0) {
        perror("Saving downloaded tracker file");
        goto cleanup;
    }
    printf("Saved %s (MD5 verified: %s)\n", output_path, local_md5);
    completed = 1;

cleanup:
    if (output != NULL) {
        fclose(output);
    }
    if (!completed) {
        unlink(temporary_path);
    }
    if (socket_fd >= 0) {
        close(socket_fd);
    }
}

static int wait_for_shutdown(unsigned int seconds)
{
    struct timespec deadline;
    int stop;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    pthread_mutex_lock(&shutdown_mutex);
    while (!shutting_down) {
        int result = pthread_cond_timedwait(&shutdown_condition, &shutdown_mutex, &deadline);
        if (result == ETIMEDOUT) {
            break;
        }
    }
    stop = shutting_down;
    pthread_mutex_unlock(&shutdown_mutex);
    return stop;
}

static void *periodic_update_thread(void *unused)
{
    (void)unused;
    while (!wait_for_shutdown(config.update_interval)) {
        DIR *directory = opendir(config.shared_dir);
        struct dirent *entry;
        if (directory == NULL) {
            perror(config.shared_dir);
            continue;
        }
        while ((entry = readdir(directory)) != NULL) {
            char path[1024];
            struct stat status;
            if (entry->d_name[0] == '.' || !safe_filename(entry->d_name) ||
                snprintf(path, sizeof(path), "%s/%s", config.shared_dir,
                         entry->d_name) >= (int)sizeof(path) ||
                stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
                continue;
            }
            (void)update_one_file(entry->d_name, 1);
        }
        closedir(directory);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    const char *config_path = argc > 1 ? argv[1] : "clientThreadConfig.cfg";
    pthread_t updater;
    int updater_started = 0;
    char choice[32];

    if (argc > 2) {
        fprintf(stderr, "Usage: %s [clientThreadConfig.cfg]\n", argv[0]);
        return EXIT_FAILURE;
    }
    (void)signal(SIGPIPE, SIG_IGN);
    if (load_config(config_path) != 0) {
        return EXIT_FAILURE;
    }
    if (ensure_directory(config.shared_dir) != 0 || ensure_directory(config.download_dir) != 0) {
        perror("Creating configured directories");
        return EXIT_FAILURE;
    }

    if (pthread_create(&updater, NULL, periodic_update_thread, NULL) == 0) {
        updater_started = 1;
        printf("Periodic tracker updates enabled (every %u seconds).\n",
               config.update_interval);
    } else {
        fprintf(stderr, "Warning: periodic update thread could not start.\n");
    }

    for (;;) {
        printf("\nPeer client (peer port %u)\n"
               "1) createtracker\n"
               "2) updatetracker\n"
               "3) LIST\n"
               "4) GET tracker file\n"
               "0) Quit\n", config.peer_port);
        if (read_prompt("Choice: ", choice, sizeof(choice)) != 0) {
            break;
        }
        if (strcmp(choice, "1") == 0) {
            command_create_tracker();
        } else if (strcmp(choice, "2") == 0) {
            command_update_tracker();
        } else if (strcmp(choice, "3") == 0) {
            command_list();
        } else if (strcmp(choice, "4") == 0) {
            command_get();
        } else if (strcmp(choice, "0") == 0) {
            break;
        } else {
            printf("Choose 0, 1, 2, 3, or 4.\n");
        }
    }

    if (updater_started) {
        pthread_mutex_lock(&shutdown_mutex);
        shutting_down = 1;
        pthread_cond_broadcast(&shutdown_condition);
        pthread_mutex_unlock(&shutdown_mutex);
        pthread_join(updater, NULL);
    }
    return EXIT_SUCCESS;
}