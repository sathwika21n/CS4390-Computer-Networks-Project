#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    protocol_request_t request;
    protocol_response_t response;
    char temporary_path[] = "./.protocol-md5-test-XXXXXX";
    char digest[PROTOCOL_MD5_HEX_LEN + 1];
    int descriptor;

    assert(protocol_parse_request(
               "<createtracker notes.txt 3 demo 900150983cd24fb0d6963f7d28e17f72 10.0.0.2 3490>",
               &request) == 0);
    assert(request.kind == PROTOCOL_REQUEST_CREATE_TRACKER);
    assert(strcmp(request.filename, "notes.txt") == 0);
    assert(request.filesize == 3 && request.port == 3490);

    assert(protocol_parse_request("<updatetracker notes.txt 0 2 10.0.0.2 3490>",
                                  &request) == 0);
    assert(request.kind == PROTOCOL_REQUEST_UPDATE_TRACKER);
    assert(request.start_bytes == 0 && request.end_bytes == 2);
    assert(protocol_parse_request("<REQ LIST>", &request) == 0);
    assert(request.kind == PROTOCOL_REQUEST_LIST);
    assert(protocol_parse_request("<GET notes.txt.track >", &request) == 0);
    assert(request.kind == PROTOCOL_REQUEST_GET);
    assert(strcmp(request.filename, "notes.txt.track") == 0);
    assert(protocol_parse_request("<GET notes.txt.track>", &request) == 0);
    assert(request.kind == PROTOCOL_REQUEST_GET);

    assert(protocol_parse_response("<createtracker succ>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_CREATE_TRACKER);
    assert(strcmp(response.status, "succ") == 0);
    assert(protocol_parse_response("<updatetracker notes.txt ferr>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_UPDATE_TRACKER);
    assert(strcmp(response.filename, "notes.txt") == 0);
    assert(strcmp(response.status, "ferr") == 0);
    assert(protocol_parse_response("<REP LIST 1>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_LIST_HEADER && response.count == 1);
    assert(protocol_parse_response(
               "<1 notes.txt 3 900150983cd24fb0d6963f7d28e17f72>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_LIST_ITEM);
    assert(response.filesize == 3 && strcmp(response.filename, "notes.txt") == 0);
    assert(protocol_parse_response("<REP LIST END>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_LIST_END);
    assert(protocol_parse_response("<REP GET BEGIN>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_GET_BEGIN);
    assert(protocol_parse_response(
               "<REP GET END 900150983cd24fb0d6963f7d28e17f72>", &response) == 0);
    assert(response.kind == PROTOCOL_RESPONSE_GET_END);

    descriptor = mkstemp(temporary_path);
    assert(descriptor >= 0);
    assert(write(descriptor, "abc", 3) == 3);
    assert(close(descriptor) == 0);
    assert(protocol_md5_file(temporary_path, digest) == 0);
    assert(strcmp(digest, "900150983cd24fb0d6963f7d28e17f72") == 0);
    if (unlink(temporary_path) != 0) {
        perror("Removing temporary MD5 fixture");
        return EXIT_FAILURE;
    }

    puts("protocol tests passed");
    return EXIT_SUCCESS;
}
