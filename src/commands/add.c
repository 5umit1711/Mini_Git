#include "commands/add.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#define mg_stat(path, info) _stat(path, info)
#define MG_STAT_STRUCT struct _stat
#define MG_IS_DIRECTORY(info) (((info).st_mode & _S_IFDIR) != 0)
#define MG_IS_REGULAR(info) (((info).st_mode & _S_IFREG) != 0)
#define mg_getcwd(buffer, size) _getcwd(buffer, (int)(size))
#define mg_fullpath(result, path, size) _fullpath(result, path, size)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define mg_stat(path, info) stat(path, info)
#define MG_STAT_STRUCT struct stat
#define MG_IS_DIRECTORY(info) S_ISDIR((info).st_mode)
#define MG_IS_REGULAR(info) S_ISREG((info).st_mode)
#define mg_getcwd(buffer, size) getcwd(buffer, size)
#define mg_fullpath(result, path, size) realpath(path, result)
#endif

#include "util/index.h"
#include "util/object.h"
#include "util/repository.h"

typedef struct {
    MgRepository repository;
    MgIndex index;
    char root[MG_PATH_MAX];
    char root_prefix[MG_PATH_MAX];
    int failed;
} AddContext;

static int is_separator(char value)
{
    return value == '/' || value == '\\';
}

static int path_is_within_root(const char *path, const char *root)
{
    size_t root_length = strlen(root);
#ifdef _WIN32
    if (_strnicmp(path, root, root_length) != 0) {
        return 0;
    }
#else
    if (strncmp(path, root, root_length) != 0) {
        return 0;
    }
#endif
    return path[root_length] == '\0' || is_separator(path[root_length]);
}

static int make_absolute(const char *path, char result[MG_PATH_MAX])
{
    char joined[MG_PATH_MAX];
    char cwd[MG_PATH_MAX];

    if (path[0] == '/' || path[0] == '\\' ||
        (strlen(path) > 2 && path[1] == ':')) {
        return mg_fullpath(result, path, MG_PATH_MAX) != NULL ? 0 : -1;
    }
    if (mg_getcwd(cwd, sizeof(cwd)) == NULL ||
        mg_path_join(joined, sizeof(joined), cwd, path) != 0) {
        return -1;
    }
    return mg_fullpath(result, joined, MG_PATH_MAX) != NULL ? 0 : -1;
}

static int read_file(const char *path, unsigned char **contents, size_t *length)
{
    FILE *file = fopen(path, "rb");
    long file_length;
    unsigned char *buffer;

    if (file == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (file_length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    buffer = malloc(file_length == 0 ? 1 : (size_t)file_length);
    if (buffer == NULL) {
        fclose(file);
        return -1;
    }
    if (fread(buffer, 1, (size_t)file_length, file) != (size_t)file_length) {
        free(buffer);
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        free(buffer);
        return -1;
    }
    *contents = buffer;
    *length = (size_t)file_length;
    return 0;
}

static int stage_file(AddContext *context, const char *absolute_path,
                      const char *relative_path)
{
    MG_STAT_STRUCT info;
    unsigned char *contents = NULL;
    size_t length = 0;
    unsigned char oid[20];
    char index_path[MG_PATH_MAX];

    if (mg_stat(absolute_path, &info) != 0 || !MG_IS_REGULAR(info)) {
        fprintf(stderr, "mygit: '%s' is not a regular file\n", relative_path);
        return -1;
    }
    if (read_file(absolute_path, &contents, &length) != 0 ||
        mg_write_blob(&context->repository, contents, length, oid) != 0) {
        free(contents);
        fprintf(stderr, "mygit: unable to store '%s'\n", relative_path);
        return -1;
    }
    free(contents);

    unsigned int mode = 0100644U;
#ifndef _WIN32
    if ((info.st_mode & S_IXUSR) != 0) {
        mode = 0100755U;
    }
#endif
    if (mg_index_set(&context->index, relative_path, oid, mode) != 0) {
        fprintf(stderr, "mygit: unable to update the index\n");
        return -1;
    }
    if (mg_path_join(index_path, sizeof(index_path),
                     context->root, relative_path) != 0) {
        return -1;
    }
    printf("added '%s'\n", relative_path);
    return 0;
}

static int stage_path(AddContext *context, const char *absolute_path,
                      const char *relative_path);

#ifdef _WIN32
static int stage_directory(AddContext *context, const char *absolute_path,
                           const char *relative_path)
{
    char pattern[MG_PATH_MAX];
    WIN32_FIND_DATAA item;
    HANDLE handle;
    int result = 0;

    if (snprintf(pattern, sizeof(pattern), "%s\\*", absolute_path) >=
        (int)sizeof(pattern)) {
        return -1;
    }
    handle = FindFirstFileA(pattern, &item);
    if (handle == INVALID_HANDLE_VALUE) {
        return -1;
    }
    do {
        char child_absolute[MG_PATH_MAX];
        char child_relative[MG_PATH_MAX];
        if (strcmp(item.cFileName, ".") == 0 ||
            strcmp(item.cFileName, "..") == 0 ||
            (relative_path[0] == '\0' &&
             strcmp(item.cFileName, ".git") == 0)) {
            continue;
        }
        if (snprintf(child_absolute, sizeof(child_absolute), "%s\\%s",
                     absolute_path, item.cFileName) >= (int)sizeof(child_absolute) ||
            (relative_path[0] == '\0'
                 ? snprintf(child_relative, sizeof(child_relative), "%s",
                            item.cFileName)
                 : snprintf(child_relative, sizeof(child_relative), "%s/%s",
                            relative_path, item.cFileName)) >=
                (int)sizeof(child_relative) ||
            stage_path(context, child_absolute, child_relative) != 0) {
            result = -1;
            break;
        }
    } while (FindNextFileA(handle, &item));
    FindClose(handle);
    return result;
}
#else
static int stage_directory(AddContext *context, const char *absolute_path,
                           const char *relative_path)
{
    DIR *directory = opendir(absolute_path);
    struct dirent *item;
    int result = 0;
    if (directory == NULL) {
        return -1;
    }
    while ((item = readdir(directory)) != NULL) {
        char child_absolute[MG_PATH_MAX];
        char child_relative[MG_PATH_MAX];
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 ||
            (relative_path[0] == '\0' && strcmp(item->d_name, ".git") == 0)) {
            continue;
        }
        if (mg_path_join(child_absolute, sizeof(child_absolute),
                         absolute_path, item->d_name) != 0 ||
            (relative_path[0] == '\0'
                 ? snprintf(child_relative, sizeof(child_relative), "%s",
                            item->d_name)
                 : snprintf(child_relative, sizeof(child_relative), "%s/%s",
                            relative_path, item->d_name)) >=
                (int)sizeof(child_relative) ||
            stage_path(context, child_absolute, child_relative) != 0) {
            result = -1;
            break;
        }
    }
    if (closedir(directory) != 0) {
        result = -1;
    }
    return result;
}
#endif

static int stage_path(AddContext *context, const char *absolute_path,
                      const char *relative_path)
{
    MG_STAT_STRUCT info;
    if (mg_stat(absolute_path, &info) != 0) {
        fprintf(stderr, "mygit: unable to access '%s'\n", relative_path);
        return -1;
    }
    if (MG_IS_DIRECTORY(info)) {
        return stage_directory(context, absolute_path, relative_path);
    }
    return stage_file(context, absolute_path, relative_path);
}

int mg_command_add(int path_count, char **pathspecs)
{
    AddContext context;
    char cwd[MG_PATH_MAX];
    int add_all = path_count == 1 &&
                  (strcmp(pathspecs[0], "-A") == 0 ||
                   strcmp(pathspecs[0], "--all") == 0);
    mg_index_init(&context.index);

    if (mg_repository_find(&context.repository) != 0) {
        fprintf(stderr, "mygit: not a MiniGit repository (or any parent directory)\n");
        return 1;
    }
    if (mg_getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("mygit: unable to determine current directory");
        return 1;
    }
    if (mg_index_read(&context.repository, &context.index) != 0) {
        fprintf(stderr, "mygit: unable to read repository index\n");
        mg_index_clear(&context.index);
        return 1;
    }

    for (int i = 0; i < (add_all ? 1 : path_count); ++i) {
        char absolute_path[MG_PATH_MAX];
        char relative_path[MG_PATH_MAX];
        size_t root_length;

        if ((add_all
                 ? snprintf(absolute_path, sizeof(absolute_path), "%s",
                            context.repository.worktree) >=
                       (int)sizeof(absolute_path)
                 : make_absolute(pathspecs[i], absolute_path) != 0) ||
            !path_is_within_root(absolute_path, context.repository.worktree)) {
            fprintf(stderr, "mygit: path '%s' is outside the repository or unavailable\n",
                    pathspecs[i]);
            mg_index_clear(&context.index);
            return 1;
        }
        root_length = strlen(context.repository.worktree);
        while (is_separator(absolute_path[root_length])) {
            ++root_length;
        }
        if (absolute_path[root_length] == '\0') {
            relative_path[0] = '\0';
        } else if (snprintf(relative_path, sizeof(relative_path), "%s",
                            absolute_path + root_length) >=
                   (int)sizeof(relative_path)) {
            fprintf(stderr, "mygit: path is too long\n");
            mg_index_clear(&context.index);
            return 1;
        }
        for (char *character = relative_path; *character != '\0'; ++character) {
            if (*character == '\\') {
                *character = '/';
            }
        }
        if (stage_path(&context, absolute_path, relative_path) != 0) {
            mg_index_clear(&context.index);
            return 1;
        }
    }

    if (add_all) {
        for (size_t i = context.index.count; i > 0; --i) {
            char absolute_path[MG_PATH_MAX];
            MG_STAT_STRUCT info;
            MgIndexEntry *entry = &context.index.entries[i - 1];
            if (mg_path_join(absolute_path, sizeof(absolute_path),
                             context.repository.worktree, entry->path) != 0) {
                fprintf(stderr, "mygit: tracked path is too long\n");
                mg_index_clear(&context.index);
                return 1;
            }
            if (mg_stat(absolute_path, &info) != 0) {
                if (errno != ENOENT && errno != ENOTDIR) {
                    fprintf(stderr, "mygit: unable to inspect '%s'\n",
                            entry->path);
                    mg_index_clear(&context.index);
                    return 1;
                }
                printf("removed '%s'\n", entry->path);
                mg_index_remove(&context.index, entry->path);
            }
        }
    }

    if (mg_index_write(&context.repository, &context.index) != 0) {
        fprintf(stderr, "mygit: unable to write repository index\n");
        mg_index_clear(&context.index);
        return 1;
    }
    mg_index_clear(&context.index);
    return 0;
}
