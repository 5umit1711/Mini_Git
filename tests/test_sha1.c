#include <stdio.h>
#include <string.h>

#include "util/sha1.h"

static int expect_digest(const char *input, const char *expected)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[20];
    char actual[41];

    mg_sha1(input, strlen(input), digest);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        actual[i * 2] = hex[digest[i] >> 4];
        actual[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    actual[40] = '\0';
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "SHA-1 mismatch: expected %s, got %s\n",
                expected, actual);
        return 1;
    }
    return 0;
}

int main(void)
{
    int result = 0;
    result |= expect_digest("", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    result |= expect_digest("abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
    const char *multi_block =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    unsigned char streamed[20];
    char streamed_hex[41];
    static const char hex[] = "0123456789abcdef";
    MgSha1 context;
    mg_sha1_init(&context);
    mg_sha1_update(&context, multi_block, 17);
    mg_sha1_update(&context, multi_block + 17, strlen(multi_block) - 17);
    mg_sha1_final(&context, streamed);
    for (size_t i = 0; i < sizeof(streamed); ++i) {
        streamed_hex[i * 2] = hex[streamed[i] >> 4];
        streamed_hex[i * 2 + 1] = hex[streamed[i] & 0x0f];
    }
    streamed_hex[40] = '\0';
    if (strcmp(streamed_hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") != 0) {
        fprintf(stderr, "streaming SHA-1 mismatch: %s\n", streamed_hex);
        result = 1;
    }
    return result;
}
