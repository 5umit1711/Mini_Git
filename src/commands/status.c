#include "commands/status.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#define mg_stat(path, info) _stat(path, info)
#define MG_STAT_STRUCT struct _stat
#define MG_IS_REGULAR(info) (((info).st_mode & _S_IFREG) != 0)
#define MG_IS_EXECUTABLE(info) 0
#else
#include <dirent.h>
#include <sys/stat.h>
#define mg_stat(path, info) stat(path, info)
#define MG_STAT_STRUCT struct stat
#define MG_IS_REGULAR(info) S_ISREG((info).st_mode)
#define MG_IS_EXECUTABLE(info) (((info).st_mode & S_IXUSR) != 0)
#endif

#include "util/index.h"
#include "util/object.h"
#include "util/repository.h"

typedef struct {
    char *path;
    unsigned char oid[20];
    unsigned int mode;
} MgHeadEntry;

typedef struct {
    MgHeadEntry *entries;
    size_t count;
    size_t capacity;
} MgHeadTree;

static int hex_to_oid(const char *hex, unsigned char oid[20])
{
    if (strlen(hex) != 40) {
        return -1;
    }
    for (size_t i = 0; i < 20; ++i) {
        int high = isdigit((unsigned char)hex[i * 2])
                       ? hex[i * 2] - '0'
                       : tolower((unsigned char)hex[i * 2]) - 'a' + 10;
        int low = isdigit((unsigned char)hex[i * 2 + 1])
                      ? hex[i * 2 + 1] - '0'
                      : tolower((unsigned char)hex[i * 2 + 1]) - 'a' + 10;
        if (!isxdigit((unsigned char)hex[i * 2]) ||
            !isxdigit((unsigned char)hex[i * 2 + 1])) {
            return -1;
        }
        oid[i] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

static int head_tree_add(MgHeadTree *tree, const char *path,
                         const unsigned char oid[20], unsigned int mode)
{
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity == 0 ? 16 : tree->capacity * 2;
        MgHeadEntry *entries = realloc(tree->entries,
                                       capacity * sizeof(*entries));
        if (entries == NULL) {
            return -1;
        }
        tree->entries = entries;
        tree->capacity = capacity;
    }
    MgHeadEntry *entry = &tree->entries[tree->count];
    entry->path = malloc(strlen(path) + 1);
    if (entry->path == NULL) {
        return -1;
    }
    strcpy(entry->path, path);
    memcpy(entry->oid, oid, 20);
    entry->mode = mode;
    ++tree->count;
    return 0;
}

static void head_tree_clear(MgHeadTree *tree)
{
    for (size_t i = 0; i < tree->count; ++i) {
        free(tree->entries[i].path);
    }
    free(tree->entries);
    memset(tree, 0, sizeof(*tree));
}

static int head_tree_find(const MgHeadTree *tree, const char *path)
{
    for (size_t i = 0; i < tree->count; ++i) {
        if (strcmp(tree->entries[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int load_tree_recursive(const MgRepository *repository,
                               const unsigned char oid[20],
                               const char *prefix, MgHeadTree *tree,
                               unsigned int depth)
{
    char type[16];
    unsigned char *data = NULL;
    size_t length = 0;
    size_t offset = 0;
    int result = -1;

    if (depth > 128 ||
        mg_read_object(repository, oid, type, &data, &length) != 0 ||
        strcmp(type, "tree") != 0) {
        free(data);
        return -1;
    }
    while (offset < length) {
        size_t mode_start = offset;
        while (offset < length && data[offset] != ' ') {
            ++offset;
        }
        if (offset == length || offset == mode_start) {
            goto cleanup;
        }
        char mode_text[16];
        size_t mode_length = offset - mode_start;
        if (mode_length >= sizeof(mode_text)) {
            goto cleanup;
        }
        memcpy(mode_text, data + mode_start, mode_length);
        mode_text[mode_length] = '\0';
        unsigned long mode = strtoul(mode_text, NULL, 8);
        ++offset;
        size_t name_start = offset;
        while (offset < length && data[offset] != '\0') {
            ++offset;
        }
        if (offset == length || offset + 21 > length) {
            goto cleanup;
        }
        size_t name_length = offset - name_start;
        char path[MG_PATH_MAX];
        int path_length = prefix[0] == '\0'
                              ? snprintf(path, sizeof(path), "%.*s",
                                         (int)name_length, data + name_start)
                              : snprintf(path, sizeof(path), "%s/%.*s", prefix,
                                         (int)name_length, data + name_start);
        ++offset;
        const unsigned char *child_oid = data + offset;
        offset += 20;
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            goto cleanup;
        }
        if (mode == 0040000U) {
            if (load_tree_recursive(repository, child_oid, path,
                                    tree, depth + 1) != 0) {
                goto cleanup;
            }
        } else if (head_tree_add(tree, path, child_oid,
                                 (unsigned int)mode) != 0) {
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(data);
    return result;
}

static int load_head_tree(const MgRepository *repository, MgHeadTree *tree,
                          char branch[MG_PATH_MAX], int *has_commit)
{
    char head_path[MG_PATH_MAX];
    char line[MG_PATH_MAX];
    char ref_path[MG_PATH_MAX];
    char oid_text[41];
    unsigned char commit_oid[20];
    unsigned char tree_oid[20];
    unsigned char *commit_data = NULL;
    size_t commit_length = 0;
    char type[16];
    FILE *file;

    *has_commit = 0;
    if (mg_path_join(head_path, sizeof(head_path),
                     repository->git_dir, "HEAD") != 0 ||
        (file = fopen(head_path, "rb")) == NULL) {
        return -1;
    }
    if (fgets(line, sizeof(line), file) == NULL) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    line[strcspn(line, "\r\n")] = '\0';

    if (strncmp(line, "ref: ", 5) == 0) {
        const char *ref = line + 5;
        if (strncmp(ref, "refs/heads/", 11) != 0 ||
            strstr(ref, "..") != NULL ||
            mg_path_join(ref_path, sizeof(ref_path),
                         repository->git_dir, ref) != 0) {
            return -1;
        }
        if (snprintf(branch, MG_PATH_MAX, "%s", ref + 11) >= MG_PATH_MAX) {
            return -1;
        }
        file = fopen(ref_path, "rb");
        if (file == NULL) {
            return errno == ENOENT ? 0 : -1;
        }
        if (fgets(oid_text, sizeof(oid_text), file) == NULL) {
            fclose(file);
            return -1;
        }
        if (fclose(file) != 0) {
            return -1;
        }
        oid_text[strcspn(oid_text, "\r\n")] = '\0';
    } else {
        if (strlen(line) != 40) {
            return -1;
        }
        snprintf(branch, MG_PATH_MAX, "HEAD detached");
        strcpy(oid_text, line);
    }
    if (hex_to_oid(oid_text, commit_oid) != 0 ||
        mg_read_object(repository, commit_oid, type,
                       &commit_data, &commit_length) != 0 ||
        strcmp(type, "commit") != 0) {
        free(commit_data);
        return -1;
    }

    size_t offset = 0;
    while (offset < commit_length && commit_data[offset] != '\n') {
        size_t line_length = 0;
        while (offset + line_length < commit_length &&
               commit_data[offset + line_length] != '\n') {
            ++line_length;
        }
        if (line_length > 5 && memcmp(commit_data + offset, "tree ", 5) == 0) {
            char tree_text[41];
            if (line_length != 45) {
                free(commit_data);
                return -1;
            }
            memcpy(tree_text, commit_data + offset + 5, 40);
            tree_text[40] = '\0';
            if (hex_to_oid(tree_text, tree_oid) != 0) {
                free(commit_data);
                return -1;
            }
            free(commit_data);
            if (load_tree_recursive(repository, tree_oid, "", tree, 0) != 0) {
                return -1;
            }
            *has_commit = 1;
            return 0;
        }
        offset += line_length + 1;
        if (line_length == 0) {
            break;
        }
    }
    free(commit_data);
    return -1;
}

static int is_tracked(const MgIndex *index, const char *path)
{
    return mg_index_find(index, path) >= 0;
}

static int report_untracked_file(const MgIndex *index, const char *path)
{
    if (is_tracked(index, path)) {
        return 0;
    }
    printf("?? %s\n", path);
    return 1;
}

#ifdef _WIN32
static int report_untracked_directory(const MgRepository *repository,
                                      const MgIndex *index,
                                      const char *absolute,
                                      const char *relative)
{
    char pattern[MG_PATH_MAX];
    WIN32_FIND_DATAA item;
    HANDLE handle;
    int count = 0;

    if (snprintf(pattern, sizeof(pattern), "%s\\*", absolute) >=
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
            (relative[0] == '\0' && strcmp(item.cFileName, ".git") == 0)) {
            continue;
        }
        if (snprintf(child_absolute, sizeof(child_absolute), "%s\\%s",
                     absolute, item.cFileName) >= (int)sizeof(child_absolute) ||
            (relative[0] == '\0'
                 ? snprintf(child_relative, sizeof(child_relative), "%s",
                            item.cFileName)
                 : snprintf(child_relative, sizeof(child_relative), "%s/%s",
                            relative, item.cFileName)) >=
                (int)sizeof(child_relative)) {
            FindClose(handle);
            return -1;
        }
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            int nested = report_untracked_directory(repository, index,
                                                   child_absolute,
                                                   child_relative);
            if (nested < 0) {
                FindClose(handle);
                return -1;
            }
            count += nested;
        } else {
            count += report_untracked_file(index, child_relative);
        }
    } while (FindNextFileA(handle, &item));
    FindClose(handle);
    (void)repository;
    return count;
}
#else
static int report_untracked_directory(const MgRepository *repository,
                                      const MgIndex *index,
                                      const char *absolute,
                                      const char *relative)
{
    DIR *directory = opendir(absolute);
    struct dirent *item;
    int count = 0;
    if (directory == NULL) {
        return -1;
    }
    while ((item = readdir(directory)) != NULL) {
        char child_absolute[MG_PATH_MAX];
        char child_relative[MG_PATH_MAX];
        struct stat info;
        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0 ||
            (relative[0] == '\0' && strcmp(item->d_name, ".git") == 0)) {
            continue;
        }
        if (mg_path_join(child_absolute, sizeof(child_absolute),
                         absolute, item->d_name) != 0 ||
            (relative[0] == '\0'
                 ? snprintf(child_relative, sizeof(child_relative), "%s",
                            item->d_name)
                 : snprintf(child_relative, sizeof(child_relative), "%s/%s",
                            relative, item->d_name)) >=
                (int)sizeof(child_relative) ||
            stat(child_absolute, &info) != 0) {
            closedir(directory);
            return -1;
        }
        if (S_ISDIR(info.st_mode)) {
            int nested = report_untracked_directory(repository, index,
                                                   child_absolute,
                                                   child_relative);
            if (nested < 0) {
                closedir(directory);
                return -1;
            }
            count += nested;
        } else if (S_ISREG(info.st_mode)) {
            count += report_untracked_file(index, child_relative);
        }
    }
    if (closedir(directory) != 0) {
        return -1;
    }
    (void)repository;
    return count;
}
#endif

static int tracked_file_status(const MgRepository *repository,
                               const MgIndexEntry *entry)
{
    char absolute_path[MG_PATH_MAX];
    MG_STAT_STRUCT info;
    unsigned char *contents;
    unsigned char oid[20];
    FILE *file;
    long length;

    if (mg_path_join(absolute_path, sizeof(absolute_path),
                     repository->worktree, entry->path) != 0) {
        return -1;
    }
    if (mg_stat(absolute_path, &info) != 0) {
        return 1;
    }
    if (!MG_IS_REGULAR(info) ||
        (file = fopen(absolute_path, "rb")) == NULL) {
        return -1;
    }
    if (fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    contents = malloc(length == 0 ? 1 : (size_t)length);
    if (contents == NULL) {
        fclose(file);
        return -1;
    }
    size_t bytes_read = fread(contents, 1, (size_t)length, file);
    int close_result = fclose(file);
    if (bytes_read != (size_t)length || close_result != 0) {
        free(contents);
        return -1;
    }
    int hash_result = mg_hash_blob(contents, (size_t)length, oid);
    free(contents);
    if (hash_result != 0) {
        return -1;
    }
    return memcmp(oid, entry->oid, 20) == 0 &&
                   MG_IS_EXECUTABLE(info) == (entry->mode == 0100755U)
               ? 0
               : 2;
}

int mg_command_status(void)
{
    MgRepository repository;
    MgIndex index;
    MgHeadTree head_tree = {0};
    char branch[MG_PATH_MAX] = "master";
    int has_commit;
    int staged_changes = 0;
    int working_changes = 0;
    int untracked_count;

    mg_index_init(&index);
    if (mg_repository_find(&repository) != 0) {
        fprintf(stderr, "mygit: not a MiniGit repository (or any parent directory)\n");
        return 1;
    }
    if (mg_index_read(&repository, &index) != 0) {
        fprintf(stderr, "mygit: unable to read repository index\n");
        mg_index_clear(&index);
        return 1;
    }
    if (load_head_tree(&repository, &head_tree, branch, &has_commit) != 0) {
        fprintf(stderr, "mygit: unable to read HEAD commit\n");
        mg_index_clear(&index);
        head_tree_clear(&head_tree);
        return 1;
    }

    printf("On branch %s\n\n", branch);
    for (size_t i = 0; i < index.count; ++i) {
        int head_entry = head_tree_find(&head_tree, index.entries[i].path);
        if (!has_commit || head_entry < 0 ||
            head_tree.entries[head_entry].mode != index.entries[i].mode ||
            memcmp(head_tree.entries[head_entry].oid,
                   index.entries[i].oid, 20) != 0) {
            staged_changes = 1;
        }
    }
    for (size_t i = 0; i < head_tree.count; ++i) {
        if (mg_index_find(&index, head_tree.entries[i].path) < 0) {
            staged_changes = 1;
        }
    }
    if (staged_changes) {
        printf("Changes to be committed:\n\n");
        for (size_t i = 0; i < index.count; ++i) {
            int head_entry = head_tree_find(&head_tree, index.entries[i].path);
            if (!has_commit || head_entry < 0) {
                printf("\tnew file:   %s\n", index.entries[i].path);
            } else if (head_tree.entries[head_entry].mode !=
                           index.entries[i].mode ||
                       memcmp(head_tree.entries[head_entry].oid,
                              index.entries[i].oid, 20) != 0) {
                printf("\tmodified:   %s\n", index.entries[i].path);
            }
        }
        for (size_t i = 0; i < head_tree.count; ++i) {
            if (mg_index_find(&index, head_tree.entries[i].path) < 0) {
                printf("\tdeleted:    %s\n", head_tree.entries[i].path);
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < index.count; ++i) {
        int state = tracked_file_status(&repository, &index.entries[i]);
        if (state < 0) {
            fprintf(stderr, "mygit: unable to inspect '%s'\n",
                    index.entries[i].path);
            mg_index_clear(&index);
            head_tree_clear(&head_tree);
            return 1;
        }
        if (state == 1 || state == 2) {
            if (!working_changes) {
                printf("Changes not staged for commit:\n\n");
            }
            printf("\t%s:   %s\n", state == 1 ? "deleted" : "modified",
                   index.entries[i].path);
            working_changes = 1;
        }
    }
    untracked_count = report_untracked_directory(&repository, &index,
                                                 repository.worktree, "");
    if (untracked_count < 0) {
        fprintf(stderr, "mygit: unable to scan working tree\n");
        mg_index_clear(&index);
        head_tree_clear(&head_tree);
        return 1;
    }
    if (untracked_count == 0 && !staged_changes && !working_changes) {
        printf(has_commit ? "nothing to commit, working tree clean\n"
                          : "No files staged yet.\n");
    }
    mg_index_clear(&index);
    head_tree_clear(&head_tree);
    return 0;
}
