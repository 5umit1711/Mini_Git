#include "util/sha1.h"

#include <string.h>

static uint32_t rotate_left(uint32_t value, unsigned int count)
{
    return (value << count) | (value >> (32U - count));
}

static void transform(MgSha1 *context, const unsigned char block[64])
{
    uint32_t words[80];
    uint32_t a, b, c, d, e;

    for (size_t i = 0; i < 16; ++i) {
        words[i] = ((uint32_t)block[i * 4] << 24) |
                   ((uint32_t)block[i * 4 + 1] << 16) |
                   ((uint32_t)block[i * 4 + 2] << 8) |
                   (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 80; ++i) {
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^
                               words[i - 14] ^ words[i - 16], 1);
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];

    for (size_t i = 0; i < 80; ++i) {
        uint32_t function;
        uint32_t constant;
        uint32_t temporary;

        if (i < 20) {
            function = (b & c) | ((~b) & d);
            constant = 0x5A827999U;
        } else if (i < 40) {
            function = b ^ c ^ d;
            constant = 0x6ED9EBA1U;
        } else if (i < 60) {
            function = (b & c) | (b & d) | (c & d);
            constant = 0x8F1BBCDCU;
        } else {
            function = b ^ c ^ d;
            constant = 0xCA62C1D6U;
        }

        temporary = rotate_left(a, 5) + function + e + constant + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = temporary;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
}

void mg_sha1_init(MgSha1 *context)
{
    context->state[0] = 0x67452301U;
    context->state[1] = 0xEFCDAB89U;
    context->state[2] = 0x98BADCFEU;
    context->state[3] = 0x10325476U;
    context->state[4] = 0xC3D2E1F0U;
    context->bit_count = 0;
    context->buffer_length = 0;
}

void mg_sha1_update(MgSha1 *context, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    context->bit_count += (uint64_t)length * 8U;

    while (length > 0) {
        size_t available = sizeof(context->buffer) - context->buffer_length;
        size_t copied = length < available ? length : available;
        memcpy(context->buffer + context->buffer_length, bytes, copied);
        context->buffer_length += copied;
        bytes += copied;
        length -= copied;
        if (context->buffer_length == sizeof(context->buffer)) {
            transform(context, context->buffer);
            context->buffer_length = 0;
        }
    }
}

void mg_sha1_final(MgSha1 *context, unsigned char digest[20])
{
    uint64_t original_bit_count = context->bit_count;
    unsigned char padding[64] = {0x80};
    unsigned char length_bytes[8];
    size_t padding_length = context->buffer_length < 56
                                ? 56 - context->buffer_length
                                : 120 - context->buffer_length;

    mg_sha1_update(context, padding, padding_length);
    for (size_t i = 0; i < 8; ++i) {
        length_bytes[7 - i] = (unsigned char)(original_bit_count >> (i * 8));
    }
    mg_sha1_update(context, length_bytes, sizeof(length_bytes));

    for (size_t i = 0; i < 5; ++i) {
        digest[i * 4] = (unsigned char)(context->state[i] >> 24);
        digest[i * 4 + 1] = (unsigned char)(context->state[i] >> 16);
        digest[i * 4 + 2] = (unsigned char)(context->state[i] >> 8);
        digest[i * 4 + 3] = (unsigned char)context->state[i];
    }
}

void mg_sha1(const void *data, size_t length, unsigned char digest[20])
{
    MgSha1 context;
    mg_sha1_init(&context);
    mg_sha1_update(&context, data, length);
    mg_sha1_final(&context, digest);
}
