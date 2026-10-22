#include "util/object.h"

#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "util/sha1.h"

static const char hex_digits[] = "0123456789abcdef";

void mg_oid_to_hex(const unsigned char oid[20], char hex[41])
{
    for (size_t i = 0; i < 20; ++i) {
        hex[i * 2] = hex_digits[oid[i] >> 4];
        hex[i * 2 + 1] = hex_digits[oid[i] & 0x0f];
    }
    hex[40] = '\0';
}

int mg_hash_blob(const void *data, size_t length, unsigned char oid[20])
{
    char header[64];
    int header_length = snprintf(header, sizeof(header), "blob %zu", length);
    MgSha1 context;

    if (header_length < 0 || (size_t)header_length + 1 > sizeof(header)) {
        return -1;
    }
    mg_sha1_init(&context);
    mg_sha1_update(&context, header, (size_t)header_length + 1);
    mg_sha1_update(&context, data, length);
    mg_sha1_final(&context, oid);
    return 0;
}

int mg_write_object(const MgRepository *repository, const char *type,
                    const void *data, size_t length, unsigned char oid[20])
{
    char hex[41];
    char object_directory[MG_PATH_MAX];
    char object_path[MG_PATH_MAX];
    char header[64];
    unsigned char *raw;
    unsigned char *compressed;
    uLongf compressed_length;
    int header_length;
    int result = -1;
    FILE *file;

    header_length = snprintf(header, sizeof(header), "%s %zu", type, length);
    if (header_length < 0 || (size_t)header_length + 1 > sizeof(header) ||
        length > SIZE_MAX - (size_t)header_length - 1) {
        return -1;
    }
    MgSha1 hash_context;
    mg_sha1_init(&hash_context);
    mg_sha1_update(&hash_context, header, (size_t)header_length + 1);
    mg_sha1_update(&hash_context, data, length);
    mg_sha1_final(&hash_context, oid);
    mg_oid_to_hex(oid, hex);
    if (mg_path_join(object_directory, sizeof(object_directory),
                     repository->git_dir, "objects") != 0 ||
        mg_path_join(object_path, sizeof(object_path),
                     object_directory, (char[3]){hex[0], hex[1], '\0'}) != 0 ||
        mg_ensure_directory(object_path) != 0) {
        return -1;
    }

    char fanout_path[MG_PATH_MAX];
    if (mg_path_join(fanout_path, sizeof(fanout_path),
                     object_directory, (char[3]){hex[0], hex[1], '\0'}) != 0 ||
        mg_path_join(object_path, sizeof(object_path),
                     fanout_path, hex + 2) != 0) {
        return -1;
    }
    file = fopen(object_path, "rb");
    if (file != NULL) {
        fclose(file);
        return 0;
    }

    size_t raw_length = (size_t)header_length + 1 + length;
    raw = malloc(raw_length == 0 ? 1 : raw_length);
    if (raw == NULL) {
        return -1;
    }
    memcpy(raw, header, (size_t)header_length + 1);
    if (length > 0) {
        memcpy(raw + (size_t)header_length + 1, data, length);
    }

    compressed_length = compressBound((uLong)raw_length);
    compressed = malloc((size_t)compressed_length);
    if (compressed == NULL) {
        free(raw);
        return -1;
    }
    if (compress2(compressed, &compressed_length, raw,
                  (uLong)raw_length, Z_BEST_COMPRESSION) != Z_OK) {
        goto cleanup;
    }

    file = fopen(object_path, "wb");
    if (file == NULL) {
        goto cleanup;
    }
    result = fwrite(compressed, 1, (size_t)compressed_length, file) ==
                     (size_t)compressed_length
                 ? 0
                 : -1;
    if (fclose(file) != 0) {
        result = -1;
    }

cleanup:
    free(compressed);
    free(raw);
    return result;
}

int mg_write_blob(const MgRepository *repository, const void *data,
                  size_t length, unsigned char oid[20])
{
    return mg_write_object(repository, "blob", data, length, oid);
}

int mg_read_object(const MgRepository *repository,
                   const unsigned char oid[20], char type[16],
                   unsigned char **data, size_t *length)
{
    char hex[41];
    char fanout[MG_PATH_MAX];
    char path[MG_PATH_MAX];
    FILE *file;
    long compressed_length;
    unsigned char *compressed = NULL;
    unsigned char *raw = NULL;
    uLongf raw_capacity;
    int result = -1;

    mg_oid_to_hex(oid, hex);
    if (mg_path_join(fanout, sizeof(fanout), repository->git_dir, "objects") != 0) {
        return -1;
    }
    char fanout_name[3] = {hex[0], hex[1], '\0'};
    if (mg_path_join(path, sizeof(path), fanout, fanout_name) != 0) {
        return -1;
    }
    char object_path[MG_PATH_MAX];
    if (mg_path_join(object_path, sizeof(object_path), path, hex + 2) != 0) {
        return -1;
    }
    file = fopen(object_path, "rb");
    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (compressed_length = ftell(file)) <= 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    compressed = malloc((size_t)compressed_length);
    if (compressed == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(compressed, 1, (size_t)compressed_length, file) !=
        (size_t)compressed_length) {
        fclose(file);
        free(compressed);
        return -1;
    }
    if (fclose(file) != 0) {
        free(compressed);
        return -1;
    }

    raw_capacity = (uLongf)((size_t)compressed_length * 4U + 1024U);
    if (raw_capacity < 1024U) {
        raw_capacity = 1024U;
    }
    for (;;) {
        raw = malloc((size_t)raw_capacity);
        if (raw == NULL) {
            goto cleanup;
        }
        uLongf actual_length = raw_capacity;
        int inflate_result = uncompress(raw, &actual_length, compressed,
                                        (uLong)compressed_length);
        if (inflate_result == Z_OK) {
            raw_capacity = actual_length;
            break;
        }
        free(raw);
        raw = NULL;
        if (inflate_result != Z_BUF_ERROR || raw_capacity > ULONG_MAX / 2U) {
            goto cleanup;
        }
        raw_capacity *= 2U;
    }

    unsigned char *separator = memchr(raw, '\0', (size_t)raw_capacity);
    if (separator == NULL) {
        goto cleanup;
    }
    unsigned char *space = memchr(raw, ' ', (size_t)(separator - raw));
    if (space == NULL || (size_t)(space - raw) >= 16 ||
        separator <= space) {
        goto cleanup;
    }
    memcpy(type, raw, (size_t)(space - raw));
    type[space - raw] = '\0';
    char *size_end = NULL;
    unsigned long long declared_length = strtoull((char *)space + 1, &size_end, 10);
    size_t header_length = (size_t)(separator - raw) + 1;
    if (size_end != (char *)separator ||
        declared_length != (unsigned long long)(raw_capacity - header_length)) {
        goto cleanup;
    }
    unsigned char computed_oid[20];
    mg_sha1(raw, (size_t)raw_capacity, computed_oid);
    if (memcmp(computed_oid, oid, sizeof(computed_oid)) != 0) {
        goto cleanup;
    }

    *length = (size_t)declared_length;
    *data = malloc(*length == 0 ? 1 : *length);
    if (*data == NULL) {
        goto cleanup;
    }
    if (*length > 0) {
        memcpy(*data, raw + header_length, *length);
    }
    result = 0;

cleanup:
    free(raw);
    free(compressed);
    return result;
}
