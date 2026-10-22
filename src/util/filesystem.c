#include "util/filesystem.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define mg_mkdir(path) _mkdir(path)
#define mg_stat(path, info) _stat(path, info)
#define MG_STAT_STRUCT struct _stat
#define MG_IS_DIRECTORY(info) (((info).st_mode & _S_IFDIR) != 0)
#else
#include <sys/stat.h>
#define mg_mkdir(path) mkdir(path, 0777)
#define mg_stat(path, info) stat(path, info)
#define MG_STAT_STRUCT struct stat
#define MG_IS_DIRECTORY(info) S_ISDIR((info).st_mode)
#endif

static int path_exists(const char *path)
{
    MG_STAT_STRUCT info;
    return mg_stat(path, &info) == 0;
}

static int append_path(char *result, size_t result_size,
                       const char *parent, const char *child)
{
    size_t parent_length = strlen(parent);
    const char *separator = "";

    if (parent_length > 0 &&
        parent[parent_length - 1] != '/' &&
        parent[parent_length - 1] != '\\') {
        separator = MG_PATH_SEPARATOR;
    }

    if (snprintf(result, result_size, "%s%s%s",
                 parent, separator, child) >= (int)result_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int mg_is_directory(const char *path)
{
    MG_STAT_STRUCT info;
    return mg_stat(path, &info) == 0 && MG_IS_DIRECTORY(info);
}

int mg_ensure_directory(const char *path)
{
    char working[MG_PATH_MAX];
    size_t length;
    size_t start = 0;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    length = strlen(path);
    if (length >= sizeof(working)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(working, path, length + 1);

    while (length > 1 &&
           (working[length - 1] == '/' || working[length - 1] == '\\')) {
        working[--length] = '\0';
    }

#ifdef _WIN32
    if (length >= 3 && working[1] == ':' &&
        (working[2] == '/' || working[2] == '\\')) {
        start = 3;
    }
#endif
    if (working[0] == '/' || working[0] == '\\') {
        start = 1;
    }

    for (size_t i = start; i < length; ++i) {
        if (working[i] != '/' && working[i] != '\\') {
            continue;
        }
        if (i == 0 || (i == 2 && working[1] == ':')) {
            continue;
        }

        char separator = working[i];
        working[i] = '\0';
        if (!path_exists(working) && mg_mkdir(working) != 0 && errno != EEXIST) {
            working[i] = separator;
            return -1;
        }
        if (!mg_is_directory(working)) {
            working[i] = separator;
            errno = ENOTDIR;
            return -1;
        }
        working[i] = separator;
    }

    if (!path_exists(working) && mg_mkdir(working) != 0 && errno != EEXIST) {
        return -1;
    }
    if (!mg_is_directory(working)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

int mg_ensure_subdirectory(const char *parent, const char *name)
{
    char path[MG_PATH_MAX];
    return append_path(path, sizeof(path), parent, name) == 0
               ? mg_ensure_directory(path)
               : -1;
}

int mg_ensure_nested_directory(const char *parent, const char *child,
                               const char *grandchild)
{
    char path[MG_PATH_MAX];
    if (append_path(path, sizeof(path), parent, child) != 0) {
        return -1;
    }
    return mg_ensure_subdirectory(path, grandchild);
}

int mg_write_file_if_missing(const char *parent, const char *name,
                             const char *contents)
{
    char path[MG_PATH_MAX];
    FILE *file;

    if (append_path(path, sizeof(path), parent, name) != 0) {
        return -1;
    }
    if (path_exists(path)) {
        return 0;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        return -1;
    }
    if (fputs(contents, file) == EOF) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    return 0;
}
