#include "util/index.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "util/sha1.h"

#define MG_INDEX_MAX_SIZE (64U * 1024U * 1024U)

static uint32_t read_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void write_be32(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static int compare_entries(const void *left, const void *right)
{
    const MgIndexEntry *a = left;
    const MgIndexEntry *b = right;
    return strcmp(a->path, b->path);
}

void mg_index_init(MgIndex *index)
{
    index->entries = NULL;
    index->count = 0;
    index->capacity = 0;
}

void mg_index_clear(MgIndex *index)
{
    for (size_t i = 0; i < index->count; ++i) {
        free(index->entries[i].path);
    }
    free(index->entries);
    mg_index_init(index);
}

int mg_index_find(const MgIndex *index, const char *path)
{
    for (size_t i = 0; i < index->count; ++i) {
        if (strcmp(index->entries[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int mg_index_remove(MgIndex *index, const char *path)
{
    int position = mg_index_find(index, path);
    if (position < 0) {
        return 0;
    }
    free(index->entries[position].path);
    if ((size_t)position + 1 < index->count) {
        memmove(&index->entries[position], &index->entries[position + 1],
                (index->count - (size_t)position - 1) *
                    sizeof(*index->entries));
    }
    --index->count;
    return 0;
}

int mg_index_set(MgIndex *index, const char *path,
                 const unsigned char oid[20], unsigned int mode)
{
    int existing = mg_index_find(index, path);
    MgIndexEntry *entry;

    if (existing >= 0) {
        entry = &index->entries[existing];
    } else {
        if (index->count == index->capacity) {
            size_t capacity = index->capacity == 0 ? 16 : index->capacity * 2;
            MgIndexEntry *entries = realloc(index->entries,
                                            capacity * sizeof(*entries));
            if (entries == NULL) {
                return -1;
            }
            index->entries = entries;
            index->capacity = capacity;
        }
        entry = &index->entries[index->count];
        entry->path = malloc(strlen(path) + 1);
        if (entry->path == NULL) {
            return -1;
        }
        strcpy(entry->path, path);
        ++index->count;
    }

    memcpy(entry->oid, oid, sizeof(entry->oid));
    entry->mode = mode;
    return 0;
}

int mg_index_read(const MgRepository *repository, MgIndex *index)
{
    char path[MG_PATH_MAX];
    unsigned char *data = NULL;
    long file_size;
    FILE *file = NULL;
    int result = -1;

    if (mg_path_join(path, sizeof(path), repository->git_dir, "index") != 0) {
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 32 ||
        (unsigned long)file_size > MG_INDEX_MAX_SIZE ||
        fseek(file, 0, SEEK_SET) != 0) {
        goto cleanup;
    }
    data = malloc((size_t)file_size);
    if (data == NULL ||
        fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        goto cleanup;
    }
    if (memcmp(data, "DIRC", 4) != 0 || read_be32(data + 4) != 2) {
        goto cleanup;
    }
    unsigned char checksum[20];
    mg_sha1(data, (size_t)file_size - 20, checksum);
    if (memcmp(checksum, data + file_size - 20, 20) != 0) {
        goto cleanup;
    }

    uint32_t entry_count = read_be32(data + 8);
    size_t offset = 12;
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (offset + 62 > (size_t)file_size - 20) {
            goto cleanup;
        }
        unsigned int mode = read_be32(data + offset + 24);
        const unsigned char *oid = data + offset + 40;
        uint16_t flags = (uint16_t)(((uint16_t)data[offset + 60] << 8) |
                                    data[offset + 61]);
        size_t path_start = offset + 62;
        size_t entry_limit = (size_t)file_size - 20;
        size_t path_end = path_start;
        while (path_end < entry_limit && data[path_end] != '\0') {
            ++path_end;
        }
        if (path_end == entry_limit ||
            (path_end - path_start < 0x0fffU &&
             path_end - path_start != (flags & 0x0fffU))) {
            goto cleanup;
        }
        size_t entry_length = path_end + 1 - offset;
        size_t padded_length = (entry_length + 7U) & ~(size_t)7U;
        if (offset + padded_length > entry_limit) {
            goto cleanup;
        }
        char *entry_path = malloc(path_end - path_start + 1);
        if (entry_path == NULL) {
            goto cleanup;
        }
        memcpy(entry_path, data + path_start, path_end - path_start);
        entry_path[path_end - path_start] = '\0';
        if (mg_index_set(index, entry_path, oid, mode) != 0) {
            free(entry_path);
            goto cleanup;
        }
        free(entry_path);
        offset += padded_length;
    }
    while (offset < (size_t)file_size - 20) {
        if (offset + 8 > (size_t)file_size - 20) {
            goto cleanup;
        }
        uint32_t extension_length = read_be32(data + offset + 4);
        if (data[offset] >= 'a' && data[offset] <= 'z') {
            goto cleanup;
        }
        offset += 8;
        if (extension_length > (size_t)file_size - 20 - offset) {
            goto cleanup;
        }
        offset += extension_length;
    }
    result = 0;

cleanup:
    free(data);
    if (file != NULL && fclose(file) != 0) {
        result = -1;
    }
    return result;
}

int mg_index_write(const MgRepository *repository, const MgIndex *index)
{
    char path[MG_PATH_MAX];
    char lock_path[MG_PATH_MAX];
    unsigned char *data;
    size_t data_size = 12 + 20;
    size_t offset = 12;
    FILE *file;
    MgIndexEntry *sorted = NULL;
    int result = -1;

    if (index->count > UINT32_MAX ||
        mg_path_join(path, sizeof(path), repository->git_dir, "index") != 0) {
        return -1;
    }

    if (index->count > 0) {
        sorted = malloc(index->count * sizeof(*sorted));
        if (sorted == NULL) {
            return -1;
        }
        memcpy(sorted, index->entries, index->count * sizeof(*sorted));
        qsort(sorted, index->count, sizeof(*sorted), compare_entries);
    }

    for (size_t i = 0; i < index->count; ++i) {
        size_t path_length = strlen(sorted[i].path);
        size_t entry_length = 62 + path_length + 1;
        size_t padded_length = (entry_length + 7U) & ~(size_t)7U;
        if (path_length > 0x0fffU || data_size > MG_INDEX_MAX_SIZE - padded_length) {
            goto cleanup;
        }
        data_size += padded_length;
    }

    data = calloc(1, data_size);
    if (data == NULL) {
        goto cleanup;
    }
    memcpy(data, "DIRC", 4);
    write_be32(data + 4, 2);
    write_be32(data + 8, (uint32_t)index->count);

    for (size_t i = 0; i < index->count; ++i) {
        size_t path_length = strlen(sorted[i].path);
        size_t entry_length = 62 + path_length + 1;
        size_t padded_length = (entry_length + 7U) & ~(size_t)7U;
        unsigned int mode = sorted[i].mode;
#ifdef _WIN32
        mode = 0100644U;
#endif
        write_be32(data + offset + 24, mode);
        memcpy(data + offset + 40, sorted[i].oid, 20);
        uint16_t flags = (uint16_t)(path_length > 0x0fffU
                                        ? 0x0fffU
                                        : path_length);
        data[offset + 60] = (unsigned char)(flags >> 8);
        data[offset + 61] = (unsigned char)flags;
        memcpy(data + offset + 62, sorted[i].path, path_length);
        offset += padded_length;
    }

    unsigned char checksum[20];
    mg_sha1(data, data_size - 20, checksum);
    memcpy(data + data_size - 20, checksum, 20);

    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >=
        (int)sizeof(lock_path)) {
        free(data);
        goto cleanup;
    }
    file = fopen(lock_path, "wbx");
    if (file == NULL) {
        free(data);
        goto cleanup;
    }
    result = fwrite(data, 1, data_size, file) == data_size ? 0 : -1;
    if (fclose(file) != 0) {
        result = -1;
    }
    free(data);
    if (result != 0) {
        remove(lock_path);
        goto cleanup;
    }
#ifdef _WIN32
    if (!MoveFileExA(lock_path, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(lock_path);
        result = -1;
    }
#else
    if (rename(lock_path, path) != 0) {
        remove(lock_path);
        result = -1;
    }
#endif

cleanup:
    free(sorted);
    return result;
}
