#include "utils.h"

// Get file size using system stat()
long getFileSize(const char *filename) {
    struct stat st;
    if (stat(filename, &st) == 0) {
        return st.st_size;
    }
    return -1;
}

// Calculate MD5 hash of a file and output as a 32-character lowercase hex string
int calculateMD5(const char *filename, char *md5_string) {
    FILE *file = fopen(filename, "rb");
    if (!file) return -1;

    MD5_CTX mdContext;
    MD5_Init(&mdContext);

    unsigned char buffer[1024];
    int bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) != 0) {
        MD5_Update(&mdContext, buffer, bytes);
    }

    unsigned char digest[MD5_DIGEST_LENGTH];
    MD5_Final(digest, &mdContext);
    fclose(file);

    // Convert binary digest to a 32-char hex string
    for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
        sprintf(&md5_string[i * 2], "%02x", (unsigned int)digest[i]);
    }
    md5_string[32] = '\0';
    return 0;
}

// Read a specific chunk of a file at a given byte offset
int readChunk(const char *filename, long offset, char *buffer, size_t size) {
    FILE *file = fopen(filename, "rb");
    if (!file) return -1;

    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    size_t bytesRead = fread(buffer, 1, size, file);
    fclose(file);
    return (int)bytesRead;
}

// Write a specific chunk of a file at a given byte offset (creates file if missing)
int writeChunk(const char *filename, long offset, const char *buffer, size_t size) {
    FILE *file = fopen(filename, "r+b");
    if (!file) {
        file = fopen(filename, "wb");
        if (!file) return -1;
    }

    if (fseek(file, offset, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }

    size_t bytesWritten = fwrite(buffer, 1, size, file);
    fclose(file);
    return (int)bytesWritten;
}

// Shared folder management
void initSharedFolder(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        mkdir(path, 0700);
        printf("[SharedFolder] Created missing directory: %s\n", path);
    } else {
        printf("[SharedFolder] Using existing directory: %s\n", path);
    }
}
