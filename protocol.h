#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_LINE_MAX 8192
#define PROTOCOL_NAME_MAX 256
#define PROTOCOL_MD5_HEX_LEN 32

typedef enum {
    PROTOCOL_RESPONSE_INVALID = 0,
    PROTOCOL_RESPONSE_CREATE_TRACKER,
    PROTOCOL_RESPONSE_UPDATE_TRACKER,
    PROTOCOL_RESPONSE_LIST_HEADER,
    PROTOCOL_RESPONSE_LIST_END,
    PROTOCOL_RESPONSE_LIST_ITEM,
    PROTOCOL_RESPONSE_GET_BEGIN,
    PROTOCOL_RESPONSE_GET_END
} protocol_response_kind_t;

typedef struct {
    protocol_response_kind_t kind;
    char status[16];
    char filename[PROTOCOL_NAME_MAX];
    char md5[PROTOCOL_MD5_HEX_LEN + 1];
    uint64_t count;
    uint64_t filesize;
    uint64_t index;
} protocol_response_t;

typedef enum {
    PROTOCOL_REQUEST_INVALID = 0,
    PROTOCOL_REQUEST_CREATE_TRACKER,
    PROTOCOL_REQUEST_UPDATE_TRACKER,
    PROTOCOL_REQUEST_LIST,
    PROTOCOL_REQUEST_GET
} protocol_request_kind_t;

typedef struct {
    protocol_request_kind_t kind;
    char filename[PROTOCOL_NAME_MAX];
    char description[PROTOCOL_NAME_MAX];
    char md5[PROTOCOL_MD5_HEX_LEN + 1];
    char ip_address[64];
    uint64_t filesize;
    uint64_t start_bytes;
    uint64_t end_bytes;
    unsigned short port;
} protocol_request_t;

/* Sends a complete newline-terminated line; returns 0 on success, -1 on error. */
int protocol_send_line(int socket_fd, const char *line);

/* Returns 1 for a line, 0 for clean EOF, and -1 for an error or oversized line. */
int protocol_read_line(int socket_fd, char *buffer, size_t buffer_size);

/* Parses one tracker response line. Returns 0 on success, -1 if malformed. */
int protocol_parse_response(const char *line, protocol_response_t *response);

/* Parses peer-to-tracker commands, for use by both client and tracker. */
int protocol_parse_request(const char *line, protocol_request_t *request);

/* Computes a lowercase hexadecimal MD5 digest for a file. */
int protocol_md5_file(const char *path, char digest[PROTOCOL_MD5_HEX_LEN + 1]);

#endif