#include "commands/branch.h"

#include <ctype.h>
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
#define mg_mkdir(path) _mkdir(path)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define mg_stat(path, info) stat(path, info)
#define MG_STAT_STRUCT struct stat
#define MG_IS_DIRECTORY(info) S_ISDIR((info).st_mode)
#define MG_IS_REGULAR(info) S_ISREG((info).st_mode)
#define mg_mkdir(path) mkdir(path, 0777)
#endif

#include "util/index.h"
#include "util/object.h"
#include "util/repository.h"

typedef struct {
    char *path;
    unsigned char oid[20];
    unsigned int mode;
} BranchFile;

typedef struct {
    BranchFile *files;
    size_t count;
    size_t capacity;
} BranchTree;

static int parse_oid(const char *hex, unsigned char oid[20])
{
    if (strlen(hex) != 40) {
        return -1;
    }
    for (size_t i = 0; i < 20; ++i) {
        int high_char = tolower((unsigned char)hex[i * 2]);
        int low_char = tolower((unsigned char)hex[i * 2 + 1]);
        int high = isdigit(high_char) ? high_char - '0' : high_char - 'a' + 10;
        int low = isdigit(low_char) ? low_char - '0' : low_char - 'a' + 10;
        if (!isxdigit((unsigned char)hex[i * 2]) ||
            !isxdigit((unsigned char)hex[i * 2 + 1])) {
            return -1;
        }
        oid[i] = (unsigned char)((high << 4) | low);
    }
    return 0;
}

static int path_is_missing_error(int error)
{
    return error == ENOENT || error == ENOTDIR;
}

static int valid_branch_name(const char *name)
{
    size_t length = strlen(name);
    if (length == 0 || length > 120 || name[0] == '-' ||
        name[0] == '.' || name[length - 1] == '.' ||
        strstr(name, "..") != NULL || strstr(name, ".lock") != NULL ||
        strcmp(name, "HEAD") == 0) {
        return 0;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (!(isalnum(c) || c == '-' || c == '_' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static int read_current_ref(const MgRepository *repository,
                            char current[MG_PATH_MAX],
                            char oid[41])
{
    char head_path[MG_PATH_MAX];
    char head[MG_PATH_MAX];
    char ref_path[MG_PATH_MAX];
    FILE *file;

    if (mg_path_join(head_path, sizeof(head_path),
                     repository->git_dir, "HEAD") != 0 ||
        (file = fopen(head_path, "rb")) == NULL) {
        return -1;
    }
    if (fgets(head, sizeof(head), file) == NULL) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    head[strcspn(head, "\r\n")] = '\0';
    if (strncmp(head, "ref: refs/heads/", 16) != 0) {
        return -1;
    }
    const char *name = head + 16;
    if (!valid_branch_name(name) ||
        snprintf(current, MG_PATH_MAX, "%s", name) >= MG_PATH_MAX ||
        mg_path_join(ref_path, sizeof(ref_path),
                     repository->git_dir, head + 5) != 0) {
        return -1;
    }
    file = fopen(ref_path, "rb");
    if (file == NULL) {
        if (errno == ENOENT) {
            oid[0] = '\0';
            return 0;
        }
        return -1;
    }
    if (fgets(oid, 41, file) == NULL) {
        fclose(file);
        return -1;
    }
    if (fclose(file) != 0) {
        return -1;
    }
    oid[strcspn(oid, "\r\n")] = '\0';
    return parse_oid(oid, (unsigned char[20]){0});
}

static int read_commit_tree(const MgRepository *repository,
                            const char *commit_hex,
                            unsigned char tree_oid[20])
{
    unsigned char commit_oid[20];
    unsigned char *data = NULL;
    size_t length = 0;
    char type[16];
    int result = -1;

    if (parse_oid(commit_hex, commit_oid) != 0 ||
        mg_read_object(repository, commit_oid, type, &data, &length) != 0 ||
        strcmp(type, "commit") != 0) {
        free(data);
        return -1;
    }
    size_t offset = 0;
    while (offset < length && data[offset] != '\n') {
        size_t line_length = 0;
        while (offset + line_length < length &&
               data[offset + line_length] != '\n') {
            ++line_length;
        }
        if (line_length == 45 && memcmp(data + offset, "tree ", 5) == 0) {
            char hex[41];
            memcpy(hex, data + offset + 5, 40);
            hex[40] = '\0';
            result = parse_oid(hex, tree_oid);
            break;
        }
        offset += line_length + 1;
    }
    free(data);
    return result;
}

static int branch_tree_add(BranchTree *tree, const char *path,
                           const unsigned char oid[20], unsigned int mode)
{
    if (tree->count == tree->capacity) {
        size_t capacity = tree->capacity == 0 ? 16 : tree->capacity * 2;
        BranchFile *files = realloc(tree->files, capacity * sizeof(*files));
        if (files == NULL) {
            return -1;
        }
        tree->files = files;
        tree->capacity = capacity;
    }
    BranchFile *file = &tree->files[tree->count];
    file->path = malloc(strlen(path) + 1);
    if (file->path == NULL) {
        return -1;
    }
    strcpy(file->path, path);
    memcpy(file->oid, oid, 20);
    file->mode = mode;
    ++tree->count;
    return 0;
}

static void branch_tree_clear(BranchTree *tree)
{
    for (size_t i = 0; i < tree->count; ++i) {
        free(tree->files[i].path);
    }
    free(tree->files);
    memset(tree, 0, sizeof(*tree));
}

static int load_tree_recursive(const MgRepository *repository,
                               const unsigned char oid[20],
                               const char *prefix, BranchTree *tree,
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
        } else if (mode == 0100644U || mode == 0100755U) {
            if (branch_tree_add(tree, path, child_oid, (unsigned int)mode) != 0) {
                goto cleanup;
            }
        } else {
            fprintf(stderr, "mygit: cannot switch branch containing unsupported file mode\n");
            goto cleanup;
        }
    }
    result = 0;

cleanup:
    free(data);
    return result;
}

static int tree_find(const BranchTree *tree, const char *path)
{
    for (size_t i = 0; i < tree->count; ++i) {
        if (strcmp(tree->files[i].path, path) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int index_matches_tree(const MgIndex *index, const BranchTree *tree)
{
    if (index->count != tree->count) {
        return 0;
    }
    for (size_t i = 0; i < index->count; ++i) {
        int position = tree_find(tree, index->entries[i].path);
        if (position < 0 ||
            index->entries[i].mode != tree->files[position].mode ||
            memcmp(index->entries[i].oid, tree->files[position].oid, 20) != 0) {
            return 0;
        }
    }
    return 1;
}

static int working_tree_matches_index(const MgRepository *repository,
                                      const MgIndex *index)
{
    for (size_t i = 0; i < index->count; ++i) {
        char path[MG_PATH_MAX];
        MG_STAT_STRUCT info;
        FILE *file;
        long length;
        unsigned char *contents;
        unsigned char oid[20];

        if (mg_path_join(path, sizeof(path), repository->worktree,
                         index->entries[i].path) != 0 ||
            mg_stat(path, &info) != 0 || !MG_IS_REGULAR(info) ||
            (file = fopen(path, "rb")) == NULL) {
            return 0;
        }
        if (fseek(file, 0, SEEK_END) != 0 ||
            (length = ftell(file)) < 0 ||
            fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return 0;
        }
        contents = malloc(length == 0 ? 1 : (size_t)length);
        if (contents == NULL) {
            fclose(file);
            return -1;
        }
        size_t bytes_read = fread(contents, 1, (size_t)length, file);
        int close_result = fclose(file);
        int hash_result = bytes_read == (size_t)length && close_result == 0
                              ? mg_hash_blob(contents, (size_t)length, oid)
                              : -1;
        free(contents);
        if (hash_result != 0) {
            return -1;
        }
        if (memcmp(oid, index->entries[i].oid, 20) != 0) {
            return 0;
        }
#ifndef _WIN32
        if (((info.st_mode & S_IXUSR) != 0) !=
            (index->entries[i].mode == 0100755U)) {
            return 0;
        }
#endif
    }
    return 1;
}

static int write_ref_atomic(const char *path, const char *contents)
{
    char lock_path[MG_PATH_MAX];
    FILE *file;
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", path) >=
        (int)sizeof(lock_path) ||
        (file = fopen(lock_path, "wbx")) == NULL) {
        return -1;
    }
    int result = fputs(contents, file) == EOF ? -1 : 0;
    if (fclose(file) != 0) {
        result = -1;
    }
    if (result != 0) {
        remove(lock_path);
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(lock_path, path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(lock_path);
        return -1;
    }
#else
    if (rename(lock_path, path) != 0) {
        remove(lock_path);
        return -1;
    }
#endif
    return 0;
}

static int create_branch(const MgRepository *repository, const char *name)
{
    char current[MG_PATH_MAX];
    char current_oid[41];
    char refs_path[MG_PATH_MAX];
    char ref_path[MG_PATH_MAX];
    char contents[42];
    FILE *file;

    if (!valid_branch_name(name)) {
        fprintf(stderr, "mygit: invalid branch name '%s'\n", name);
        return 2;
    }
    if (read_current_ref(repository, current, current_oid) != 0 ||
        current_oid[0] == '\0') {
        fprintf(stderr, "mygit: cannot create a branch before the first commit\n");
        return 1;
    }
    if (mg_path_join(refs_path, sizeof(refs_path), repository->git_dir,
                     "refs/heads") != 0 ||
        mg_path_join(ref_path, sizeof(ref_path), refs_path, name) != 0) {
        fprintf(stderr, "mygit: branch name is too long\n");
        return 1;
    }
    file = fopen(ref_path, "rb");
    if (file != NULL) {
        fclose(file);
        fprintf(stderr, "mygit: branch '%s' already exists\n", name);
        return 1;
    }
    if (snprintf(contents, sizeof(contents), "%s\n", current_oid) >=
        (int)sizeof(contents) ||
        write_ref_atomic(ref_path, contents) != 0) {
        fprintf(stderr, "mygit: unable to create branch '%s'\n", name);
        return 1;
    }
    printf("Created branch '%s'\n", name);
    return 0;
}

static int list_branches(const MgRepository *repository)
{
    char current[MG_PATH_MAX];
    char current_oid[41];
    char refs_path[MG_PATH_MAX];
    if (read_current_ref(repository, current, current_oid) != 0 ||
        mg_path_join(refs_path, sizeof(refs_path),
                     repository->git_dir, "refs/heads") != 0) {
        fprintf(stderr, "mygit: unable to read branch references\n");
        return 1;
    }
#ifdef _WIN32
    char pattern[MG_PATH_MAX];
    WIN32_FIND_DATAA item;
    if (snprintf(pattern, sizeof(pattern), "%s\\*", refs_path) >=
        (int)sizeof(pattern)) {
        return 1;
    }
    HANDLE handle = FindFirstFileA(pattern, &item);
    if (handle == INVALID_HANDLE_VALUE) {
        return 1;
    }
    do {
        if (strcmp(item.cFileName, ".") == 0 ||
            strcmp(item.cFileName, "..") == 0 ||
            (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        printf("%c %s\n", strcmp(item.cFileName, current) == 0 ? '*' : ' ',
               item.cFileName);
    } while (FindNextFileA(handle, &item));
    FindClose(handle);
#else
    DIR *directory = opendir(refs_path);
    struct dirent *item;
    if (directory == NULL) {
        return 1;
    }
    while ((item = readdir(directory)) != NULL) {
        char path[MG_PATH_MAX];
        MG_STAT_STRUCT info;
        if (item->d_name[0] == '.' ||
            mg_path_join(path, sizeof(path), refs_path, item->d_name) != 0 ||
            mg_stat(path, &info) != 0 || !MG_IS_REGULAR(info)) {
            continue;
        }
        printf("%c %s\n", strcmp(item->d_name, current) == 0 ? '*' : ' ',
               item->d_name);
    }
    if (closedir(directory) != 0) {
        return 1;
    }
#endif
    if (current_oid[0] == '\0') {
        printf("  (HEAD detached or unborn)\n");
    }
    return 0;
}

static int ensure_parent_directories(const char *path)
{
    char parent[MG_PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof(parent)) {
        return -1;
    }
    memcpy(parent, path, length + 1);
    for (size_t i = length; i > 0; --i) {
        if (parent[i - 1] == '/' || parent[i - 1] == '\\') {
            parent[i - 1] = '\0';
            break;
        }
    }
    if (parent[0] == '\0') {
        return 0;
    }
    for (size_t i = 1; parent[i] != '\0'; ++i) {
        if (parent[i] == '/' || parent[i] == '\\') {
            char separator = parent[i];
            parent[i] = '\0';
            MG_STAT_STRUCT info;
            if (mg_stat(parent, &info) != 0 && mg_mkdir(parent) != 0 &&
                errno != EEXIST) {
                return -1;
            }
            parent[i] = separator;
        }
    }
    MG_STAT_STRUCT info;
    if (mg_stat(parent, &info) != 0 && mg_mkdir(parent) != 0 &&
        errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int write_worktree_file(const MgRepository *repository,
                               const BranchFile *entry)
{
    char path[MG_PATH_MAX];
    char type[16];
    unsigned char *data = NULL;
    size_t length = 0;
    FILE *file;
    int result = -1;

    if (mg_path_join(path, sizeof(path), repository->worktree,
                     entry->path) != 0 ||
        mg_read_object(repository, entry->oid, type, &data, &length) != 0 ||
        strcmp(type, "blob") != 0 ||
        ensure_parent_directories(path) != 0) {
        free(data);
        return -1;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        free(data);
        return -1;
    }
    result = fwrite(data, 1, length, file) == length ? 0 : -1;
    if (fclose(file) != 0) {
        result = -1;
    }
    free(data);
#ifndef _WIN32
    if (result == 0 &&
        chmod(path, entry->mode == 0100755U ? 0755 : 0644) != 0) {
        result = -1;
    }
#endif
    return result;
}

static int switch_branch(const MgRepository *repository, const char *name)
{
    char current[MG_PATH_MAX];
    char current_oid[41];
    char refs_path[MG_PATH_MAX];
    char target_path[MG_PATH_MAX];
    char target_oid[41];
    char head_path[MG_PATH_MAX];
    char head_contents[MG_PATH_MAX + 16];
    unsigned char current_tree_oid[20];
    unsigned char target_tree_oid[20];
    MgIndex index;
    BranchTree current_tree = {0};
    BranchTree target_tree = {0};
    int result = 1;

    if (!valid_branch_name(name)) {
        fprintf(stderr, "mygit: invalid branch name '%s'\n", name);
        return 2;
    }
    if (read_current_ref(repository, current, current_oid) != 0) {
        fprintf(stderr, "mygit: unable to read current branch\n");
        return 1;
    }
    if (strcmp(name, current) == 0) {
        printf("Already on '%s'\n", name);
        return 0;
    }
    if (mg_path_join(refs_path, sizeof(refs_path),
                     repository->git_dir, "refs/heads") != 0 ||
        mg_path_join(target_path, sizeof(target_path), refs_path, name) != 0) {
        return 1;
    }
    FILE *file = fopen(target_path, "rb");
    if (file == NULL) {
        fprintf(stderr, "mygit: branch '%s' does not exist\n", name);
        return 1;
    }
    if (fgets(target_oid, sizeof(target_oid), file) == NULL) {
        fclose(file);
        return 1;
    }
    if (fclose(file) != 0) {
        return 1;
    }
    target_oid[strcspn(target_oid, "\r\n")] = '\0';
    if (strlen(target_oid) != 40 ||
        read_commit_tree(repository, target_oid, target_tree_oid) != 0 ||
        load_tree_recursive(repository, target_tree_oid, "", &target_tree, 0) != 0) {
        fprintf(stderr, "mygit: target branch has an invalid commit or tree\n");
        goto cleanup;
    }
    mg_index_init(&index);
    if (mg_index_read(repository, &index) != 0) {
        fprintf(stderr, "mygit: unable to read repository index\n");
        mg_index_clear(&index);
        goto cleanup;
    }

    int has_current_commit = current_oid[0] != '\0';
    if (has_current_commit) {
        if (read_commit_tree(repository, current_oid, current_tree_oid) != 0 ||
            load_tree_recursive(repository, current_tree_oid, "",
                                &current_tree, 0) != 0) {
            fprintf(stderr, "mygit: current branch has an invalid commit\n");
            mg_index_clear(&index);
            goto cleanup;
        }
    }
    int index_clean = has_current_commit
                          ? index_matches_tree(&index, &current_tree)
                          : index.count == 0;
    int worktree_clean = working_tree_matches_index(repository, &index);
    if (worktree_clean < 0) {
        fprintf(stderr, "mygit: unable to inspect working tree\n");
        mg_index_clear(&index);
        goto cleanup;
    }
    if (!index_clean || !worktree_clean) {
        fprintf(stderr, "mygit: commit or discard local changes before switching branches\n");
        mg_index_clear(&index);
        goto cleanup;
    }

    for (size_t i = 0; i < target_tree.count; ++i) {
        char path[MG_PATH_MAX];
        MG_STAT_STRUCT info;
        if (mg_path_join(path, sizeof(path), repository->worktree,
                         target_tree.files[i].path) != 0) {
            mg_index_clear(&index);
            goto cleanup;
        }
        if (mg_stat(path, &info) == 0) {
            if (tree_find(&current_tree, target_tree.files[i].path) < 0) {
                fprintf(stderr,
                        "mygit: switching would overwrite untracked file '%s'\n",
                        target_tree.files[i].path);
                mg_index_clear(&index);
                goto cleanup;
            }
        } else if (!path_is_missing_error(errno)) {
            fprintf(stderr, "mygit: unable to inspect '%s'\n",
                    target_tree.files[i].path);
            mg_index_clear(&index);
            goto cleanup;
        }
        const char *slash = target_tree.files[i].path;
        while ((slash = strchr(slash, '/')) != NULL) {
            size_t prefix_length = (size_t)(slash - target_tree.files[i].path);
            char prefix[MG_PATH_MAX];
            char parent_path[MG_PATH_MAX];
            if (prefix_length >= sizeof(prefix)) {
                mg_index_clear(&index);
                goto cleanup;
            }
            memcpy(prefix, target_tree.files[i].path, prefix_length);
            prefix[prefix_length] = '\0';
            if (mg_path_join(parent_path, sizeof(parent_path),
                             repository->worktree, prefix) != 0) {
                mg_index_clear(&index);
                goto cleanup;
            }
            if (mg_stat(parent_path, &info) == 0) {
                if (!MG_IS_DIRECTORY(info) &&
                    tree_find(&current_tree, prefix) < 0) {
                    fprintf(stderr,
                            "mygit: switching would overwrite untracked path '%s'\n",
                            prefix);
                    mg_index_clear(&index);
                    goto cleanup;
                }
            } else if (!path_is_missing_error(errno)) {
                fprintf(stderr, "mygit: unable to inspect '%s'\n", prefix);
                mg_index_clear(&index);
                goto cleanup;
            }
            ++slash;
        }
    }

    for (size_t i = 0; i < current_tree.count; ++i) {
        if (tree_find(&target_tree, current_tree.files[i].path) < 0) {
            char path[MG_PATH_MAX];
            if (mg_path_join(path, sizeof(path), repository->worktree,
                             current_tree.files[i].path) != 0 ||
                (remove(path) != 0 && errno != ENOENT)) {
                fprintf(stderr, "mygit: unable to remove '%s' during switch\n",
                        current_tree.files[i].path);
                mg_index_clear(&index);
                goto cleanup;
            }
        }
    }
    for (size_t i = 0; i < target_tree.count; ++i) {
        if (write_worktree_file(repository, &target_tree.files[i]) != 0 ||
            mg_index_set(&index, target_tree.files[i].path,
                         target_tree.files[i].oid,
                         target_tree.files[i].mode) != 0) {
            fprintf(stderr, "mygit: unable to check out '%s'\n",
                    target_tree.files[i].path);
            mg_index_clear(&index);
            goto cleanup;
        }
    }
    for (size_t i = index.count; i > 0; --i) {
        if (tree_find(&target_tree, index.entries[i - 1].path) < 0) {
            mg_index_remove(&index, index.entries[i - 1].path);
        }
    }
    if (mg_index_write(repository, &index) != 0 ||
        mg_path_join(head_path, sizeof(head_path),
                     repository->git_dir, "HEAD") != 0 ||
        snprintf(head_contents, sizeof(head_contents),
                 "ref: refs/heads/%s\n", name) >= (int)sizeof(head_contents) ||
        write_ref_atomic(head_path, head_contents) != 0) {
        fprintf(stderr, "mygit: unable to update HEAD or index\n");
        mg_index_clear(&index);
        goto cleanup;
    }
    mg_index_clear(&index);
    printf("Switched to branch '%s'\n", name);
    result = 0;

cleanup:
    branch_tree_clear(&current_tree);
    branch_tree_clear(&target_tree);
    return result;
}

int mg_command_branch(int argc, char **argv)
{
    MgRepository repository;
    if (mg_repository_find(&repository) != 0) {
        fprintf(stderr, "mygit: not a MiniGit repository (or any parent directory)\n");
        return 1;
    }
    if (argc == 0) {
        return list_branches(&repository);
    }
    if (argc == 1) {
        return create_branch(&repository, argv[0]);
    }
    if (argc == 2 && strcmp(argv[0], "--switch") == 0) {
        return switch_branch(&repository, argv[1]);
    }
    fprintf(stderr, "mygit: usage: mygit branch [name] | branch --switch <name>\n");
    return 2;
}
