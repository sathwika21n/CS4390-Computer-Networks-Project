#include "../downloader.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t number(const char *s) {
    char *end;
    errno = 0;
    uintmax_t n = strtoumax(s, &end, 10);
    if (!*s || *s < '0' || *s > '9' || *end || errno || n > UINT64_MAX) {
        fprintf(stderr, "Invalid unsigned integer: %s\n", s);
        exit(2);
    }
    return (uint64_t)n;
}
int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "Usage: %s HOST PORT NAME OUTPUT SIZE CHUNK\n", argv[0]);
        return 2;
    }
    uint64_t port = number(argv[2]), chunk = number(argv[6]);
    if (!port || port > UINT16_MAX || !chunk || chunk > SIZE_MAX) return 2;
    dl_options o = {argv[1], (uint16_t)port, argv[3], argv[4], number(argv[5]), (size_t)chunk};
    char error[256];
    int result = dl_download(&o, error, sizeof(error));
    if (result) fprintf(stderr, "%s\n", error);
    return result ? 1 : 0;
}
