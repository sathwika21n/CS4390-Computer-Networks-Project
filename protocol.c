#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

static int send_all(int socket_fd, const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(socket_fd, data + sent, length - sent, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            return -1;
        }
        sent += (size_t)result;
    }
    return 0;
}

int protocol_send_line(int socket_fd, const char *line)
{
    size_t length;

    if (line == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strlen(line);
    if (send_all(socket_fd, line, length) != 0) {
        return -1;
    }
    if (length == 0 || line[length - 1] != '\n') {
        return send_all(socket_fd, "\n", 1);
    }
    return 0;
}

int protocol_read_line(int socket_fd, char *buffer, size_t buffer_size)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size < 2) {
        errno = EINVAL;
        return -1;
    }

    while (used + 1 < buffer_size) {
        char character;
        ssize_t result = recv(socket_fd, &character, 1, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (result == 0) {
            if (used == 0) {
                return 0;
            }
            break;
        }
        if (character == '\n') {
            break;
        }
        if (character != '\r') {
            buffer[used++] = character;
        }
    }

    if (used + 1 >= buffer_size) {
        errno = EMSGSIZE;
        return -1;
    }
    buffer[used] = '\0';
    return 1;
}

static int valid_md5(const char *value)
{
    size_t index;
    if (strlen(value) != PROTOCOL_MD5_HEX_LEN) {
        return 0;
    }
    for (index = 0; index < PROTOCOL_MD5_HEX_LEN; index++) {
        if (!isxdigit((unsigned char)value[index])) {
            return 0;
        }
    }
    return 1;
}

int protocol_parse_response(const char *line, protocol_response_t *response)
{
    char command[32] = {0};
    char first[PROTOCOL_NAME_MAX] = {0};
    char second[PROTOCOL_NAME_MAX] = {0};
    char third[PROTOCOL_NAME_MAX] = {0};
    char extra[2] = {0};
    unsigned long long number;
    int fields;

    if (line == NULL || response == NULL) {
        return -1;
    }
    memset(response, 0, sizeof(*response));

    fields = sscanf(line, " <%31s %255s %255s %255s %1s", command, first,
                    second, third, extra);
    if (fields < 1) {
        return -1;
    }

    if (strcmp(command, "createtracker") == 0 && fields == 2 &&
        (strcmp(first, "succ>") == 0 || strcmp(first, "fail>") == 0 ||
         strcmp(first, "ferr>") == 0)) {
        response->kind = PROTOCOL_RESPONSE_CREATE_TRACKER;
        snprintf(response->status, sizeof(response->status), "%s", first);
        response->status[strlen(response->status) - 1] = '\0';
        return 0;
    }

    if (strcmp(command, "updatetracker") == 0 && fields == 3 &&
        (strcmp(second, "succ>") == 0 || strcmp(second, "fail>") == 0 ||
         strcmp(second, "ferr>") == 0)) {
        response->kind = PROTOCOL_RESPONSE_UPDATE_TRACKER;
        snprintf(response->filename, sizeof(response->filename), "%s", first);
        snprintf(response->status, sizeof(response->status), "%s", second);
        response->status[strlen(response->status) - 1] = '\0';
        return 0;
    }

    if (strcmp(command, "REP") == 0 && strcmp(first, "LIST") == 0 && fields == 3) {
        if (strcmp(second, "END>") == 0) {
            response->kind = PROTOCOL_RESPONSE_LIST_END;
            return 0;
        }
        if (sscanf(second, "%llu", &number) == 1 && strcmp(third, "") == 0) {
            response->kind = PROTOCOL_RESPONSE_LIST_HEADER;
            response->count = (uint64_t)number;
            return 0;
        }
    }

    /* A list item has the form: <index filename filesize md5>. */
    fields = sscanf(line, " <%llu %255s %llu %32[0-9a-fA-F]>%1s", &number, first,
                    &response->filesize, response->md5, extra);
    if (fields == 4 && valid_md5(response->md5)) {
        response->kind = PROTOCOL_RESPONSE_LIST_ITEM;
        response->index = (uint64_t)number;
        snprintf(response->filename, sizeof(response->filename), "%s", first);
        return 0;
    }

    if (strcmp(line, "<REP GET BEGIN>") == 0) {
        response->kind = PROTOCOL_RESPONSE_GET_BEGIN;
        return 0;
    }

    fields = sscanf(line, "<REP GET END %32[0-9a-fA-F]>%1s", response->md5, extra);
    if (fields == 1 && valid_md5(response->md5)) {
        response->kind = PROTOCOL_RESPONSE_GET_END;
        return 0;
    }
    return -1;
}

int protocol_parse_request(const char *line, protocol_request_t *request)
{
    char filename[PROTOCOL_NAME_MAX] = {0};
    char description[PROTOCOL_NAME_MAX] = {0};
    char md5[PROTOCOL_MD5_HEX_LEN + 1] = {0};
    char ip_address[64] = {0};
    char extra[2] = {0};
    unsigned long long first_number;
    unsigned long long second_number;
    unsigned int port;
    int fields;

    if (line == NULL || request == NULL) {
        return -1;
    }
    memset(request, 0, sizeof(*request));

    fields = sscanf(line, "<createtracker %255s %llu %255s %32s %63s %u>%1s",
                    filename, &first_number, description, md5, ip_address, &port, extra);
    if (fields == 6 && valid_md5(md5) && port > 0 && port <= 65535) {
        request->kind = PROTOCOL_REQUEST_CREATE_TRACKER;
        snprintf(request->filename, sizeof(request->filename), "%s", filename);
        snprintf(request->description, sizeof(request->description), "%s", description);
        snprintf(request->md5, sizeof(request->md5), "%s", md5);
        snprintf(request->ip_address, sizeof(request->ip_address), "%s", ip_address);
        request->filesize = (uint64_t)first_number;
        request->port = (unsigned short)port;
        return 0;
    }

    fields = sscanf(line, "<updatetracker %255s %llu %llu %63s %u>%1s",
                    filename, &first_number, &second_number, ip_address, &port, extra);
    if (fields == 5 && port > 0 && port <= 65535 &&
        (first_number <= second_number || (first_number == 0 && second_number == 0))) {
        request->kind = PROTOCOL_REQUEST_UPDATE_TRACKER;
        snprintf(request->filename, sizeof(request->filename), "%s", filename);
        snprintf(request->ip_address, sizeof(request->ip_address), "%s", ip_address);
        request->start_bytes = (uint64_t)first_number;
        request->end_bytes = (uint64_t)second_number;
        request->port = (unsigned short)port;
        return 0;
    }

    if (strcmp(line, "<REQ LIST>") == 0) {
        request->kind = PROTOCOL_REQUEST_LIST;
        return 0;
    }

    fields = sscanf(line, "<GET %255s >%1s", filename, extra);
    if (fields == 1 && filename[0] != '\0') {
        request->kind = PROTOCOL_REQUEST_GET;
        snprintf(request->filename, sizeof(request->filename), "%s", filename);
        return 0;
    }
    return -1;
}

#if defined(__APPLE__) && defined(__clang__)
/* MD5 is required by the course wire protocol as an integrity checksum. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
int protocol_md5_file(const char *path, char digest[PROTOCOL_MD5_HEX_LEN + 1])
{
    FILE *file;
    unsigned char buffer[8192];
    unsigned char hash[PROTOCOL_MD5_HEX_LEN];
    static const char hex_digits[] = "0123456789abcdef";
    size_t bytes_read;
    size_t index;
#ifdef __APPLE__
    CC_MD5_CTX context;
#else
    EVP_MD_CTX *context;
    unsigned int hash_length = 0;
#endif

    if (path == NULL || digest == NULL) {
        errno = EINVAL;
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return -1;
    }
#ifdef __APPLE__
    if (CC_MD5_Init(&context) == 0) {
        fclose(file);
        errno = EIO;
        return -1;
    }
#else
    context = EVP_MD_CTX_new();
    if (context == NULL || EVP_DigestInit_ex(context, EVP_md5(), NULL) != 1) {
        EVP_MD_CTX_free(context);
        fclose(file);
        errno = EIO;
        return -1;
    }
#endif

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
#ifdef __APPLE__
        if (CC_MD5_Update(&context, buffer, (CC_LONG)bytes_read) == 0) {
            fclose(file);
            errno = EIO;
            return -1;
        }
#else
        if (EVP_DigestUpdate(context, buffer, bytes_read) != 1) {
            EVP_MD_CTX_free(context);
            fclose(file);
            errno = EIO;
            return -1;
        }
#endif
    }
    if (ferror(file)) {
#ifndef __APPLE__
        EVP_MD_CTX_free(context);
#endif
        fclose(file);
        errno = EIO;
        return -1;
    }
#ifdef __APPLE__
    if (CC_MD5_Final(hash, &context) == 0) {
        fclose(file);
        errno = EIO;
        return -1;
    }
#else
    if (EVP_DigestFinal_ex(context, hash, &hash_length) != 1 ||
        hash_length != PROTOCOL_MD5_HEX_LEN / 2) {
        EVP_MD_CTX_free(context);
        fclose(file);
        errno = EIO;
        return -1;
    }
    EVP_MD_CTX_free(context);
#endif
    fclose(file);

    for (index = 0; index < PROTOCOL_MD5_HEX_LEN / 2; index++) {
        digest[index * 2] = hex_digits[hash[index] >> 4];
        digest[index * 2 + 1] = hex_digits[hash[index] & 0x0f];
    }
    digest[PROTOCOL_MD5_HEX_LEN] = '\0';
    return 0;
}
#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic pop
#endif
