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

/* Blocking, single-threaded. Chunk size must be 1 through 1024 bytes.
 * Returns 0 on success, -1 with a diagnostic.
 * Refuses existing destinations. On failure a partial output may remain;
 * inspect/remove it before retrying. No resume support in this increment. */
int dl_download(const dl_options *options, char *error, size_t error_size);

/* Download the file named in verified tracker metadata into an existing directory.
 * Returns 0 only after size and MD5 verification, or -1 with a diagnostic.
 * Repeat the call with fresh metadata to resume missing chunks. */
int dl_download_track(const char *tracker_path, const char *directory,
                      char *error, size_t error_size);

/* Publish the verified payload in both the download and serving directories. */
int dl_download_shared(const char *tracker_path, const char *directory,
                       const char *shared_directory, char *error, size_t error_size);

typedef struct { uint64_t first, last; } dl_range;

/* Snapshot of durable, inclusive ranges. The caller frees the returned array. */
int dl_partial_ranges(const char *directory, const char *name,
                      dl_range **ranges, size_t *count);
int dl_read_partial(const char *directory, const char *name, uint64_t offset,
                    void *buffer, size_t length);
int dl_source_signature(const char *tracker_path, const char *own_ip,
                        uint16_t own_port, uint64_t *signature);
#endif
