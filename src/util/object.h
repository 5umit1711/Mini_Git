#ifndef MYGIT_UTIL_OBJECT_H
#define MYGIT_UTIL_OBJECT_H

#include <stddef.h>

#include "util/repository.h"

int mg_write_blob(const MgRepository *repository, const void *data,
                  size_t length, unsigned char oid[20]);
int mg_write_object(const MgRepository *repository, const char *type,
                    const void *data, size_t length,
                    unsigned char oid[20]);
int mg_read_object(const MgRepository *repository,
                   const unsigned char oid[20], char type[16],
                   unsigned char **data, size_t *length);
int mg_hash_blob(const void *data, size_t length, unsigned char oid[20]);
void mg_oid_to_hex(const unsigned char oid[20], char hex[41]);

#endif
