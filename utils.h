#ifndef UTILS_H
#define UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>

// File size & hash utilities
long getFileSize(const char *filename);
int calculateMD5(const char *filename, char *md5_string);

// Chunk read/write utilities
int readChunk(const char *filename, long offset, char *buffer, size_t size);
int writeChunk(const char *filename, long offset, const char *buffer, size_t size);

// Shared folder utility
void initSharedFolder(const char *path);

#endif
