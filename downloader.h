#ifndef DOWNLOADER_H
#define DOWNLOADER_H
#include <stddef.h>
#include <stdint.h>

/* One peer serves the complete file. All offsets are zero-based byte offsets. */
typedef struct {
    const char *host;
    uint16_t port;
    const char *remote_name;
    const char *output_path;
    uint64_t file_size;
    size_t chunk_size;
} dl_options;

/* Blocking, single-threaded. Returns 0 on success, -1 with a diagnostic.
 * Refuses existing destinations. On failure a partial output may remain;
 * inspect/remove it before retrying. No resume support in this increment. */
int dl_download(const dl_options *options, char *error, size_t error_size);
#endif
