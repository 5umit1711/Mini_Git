#ifndef MYGIT_UTIL_FILESYSTEM_H
#define MYGIT_UTIL_FILESYSTEM_H

#include <stddef.h>

#define MG_PATH_MAX 4096

#ifdef _WIN32
#define MG_PATH_SEPARATOR "\\"
#else
#define MG_PATH_SEPARATOR "/"
#endif

int mg_is_directory(const char *path);
int mg_ensure_directory(const char *path);
int mg_ensure_subdirectory(const char *parent, const char *name);
int mg_ensure_nested_directory(const char *parent, const char *child,
                               const char *grandchild);
int mg_write_file_if_missing(const char *parent, const char *name,
                             const char *contents);

#endif
