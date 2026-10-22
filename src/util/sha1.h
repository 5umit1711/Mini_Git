#ifndef MYGIT_UTIL_SHA1_H
#define MYGIT_UTIL_SHA1_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint64_t bit_count;
    unsigned char buffer[64];
    size_t buffer_length;
} MgSha1;

void mg_sha1_init(MgSha1 *context);
void mg_sha1_update(MgSha1 *context, const void *data, size_t length);
void mg_sha1_final(MgSha1 *context, unsigned char digest[20]);
void mg_sha1(const void *data, size_t length, unsigned char digest[20]);

#endif
