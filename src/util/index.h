#ifndef MYGIT_UTIL_INDEX_H
#define MYGIT_UTIL_INDEX_H

#include <stddef.h>

#include "util/repository.h"

typedef struct {
    char *path;
    unsigned char oid[20];
    unsigned int mode;
} MgIndexEntry;

typedef struct {
    MgIndexEntry *entries;
    size_t count;
    size_t capacity;
} MgIndex;

void mg_index_init(MgIndex *index);
void mg_index_clear(MgIndex *index);
int mg_index_read(const MgRepository *repository, MgIndex *index);
int mg_index_write(const MgRepository *repository, const MgIndex *index);
int mg_index_set(MgIndex *index, const char *path,
                 const unsigned char oid[20], unsigned int mode);
int mg_index_remove(MgIndex *index, const char *path);
int mg_index_find(const MgIndex *index, const char *path);

#endif
