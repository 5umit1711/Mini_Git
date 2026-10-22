#include "commands/commit.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "util/index.h"
#include "util/object.h"
#include "util/repository.h"

typedef struct MgTreeNode MgTreeNode;

typedef struct {
    char *name;
    int is_tree;
    unsigned int mode;
    unsigned char oid[20];
    MgTreeNode *tree;
} MgTreeEntry;

struct MgTreeNode {
    MgTreeEntry *entries;
    size_t count;
    size_t capacity;
};

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
} MgBuffer;

static void tree_clear(MgTreeNode *node)
{
    for (size_t i = 0; i < node->count; ++i) {
        free(node->entries[i].name);
        if (node->entries[i].tree != NULL) {
            tree_clear(node->entries[i].tree);
            free(node->entries[i].tree);
        }
    }
    free(node->entries);
    node->entries = NULL;
    node->count = 0;
    node->capacity = 0;
}

static MgTreeEntry *tree_get_or_add(MgTreeNode *node, const char *name)
{
    for (size_t i = 0; i < node->count; ++i) {
        if (strcmp(node->entries[i].name, name) == 0) {
            return &node->entries[i];
        }
    }
    if (node->count == node->capacity) {
        size_t capacity = node->capacity == 0 ? 8 : node->capacity * 2;
        MgTreeEntry *entries = realloc(node->entries,
                                       capacity * sizeof(*entries));
        if (entries == NULL) {
            return NULL;
        }
        node->entries = entries;
        node->capacity = capacity;
    }
    MgTreeEntry *entry = &node->entries[node->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = malloc(strlen(name) + 1);
    if (entry->name == NULL) {
        return NULL;
    }
    strcpy(entry->name, name);
    ++node->count;
    return entry;
}

static int tree_insert(MgTreeNode *root, const MgIndexEntry *index_entry)
{
    const char *cursor = index_entry->path;
    MgTreeNode *node = root;

    while (*cursor != '\0') {
        const char *slash = strchr(cursor, '/');
        size_t name_length = slash == NULL
                                 ? strlen(cursor)
                                 : (size_t)(slash - cursor);
        char *name = malloc(name_length + 1);
        if (name == NULL) {
            return -1;
        }
        memcpy(name, cursor, name_length);
        name[name_length] = '\0';
        MgTreeEntry *entry = tree_get_or_add(node, name);
        free(name);
        if (entry == NULL) {
            return -1;
        }

        if (slash == NULL) {
            if (entry->is_tree) {
                return -1;
            }
            entry->is_tree = 0;
            entry->mode = index_entry->mode;
            memcpy(entry->oid, index_entry->oid, sizeof(entry->oid));
            return 0;
        }
        if (entry->tree == NULL) {
            entry->tree = calloc(1, sizeof(*entry->tree));
            if (entry->tree == NULL) {
                return -1;
            }
        }
        entry->is_tree = 1;
        node = entry->tree;
        cursor = slash + 1;
    }
    return -1;
}

static int tree_entry_compare(const void *left, const void *right)
{
    const MgTreeEntry *a = left;
    const MgTreeEntry *b = right;
    size_t a_length = strlen(a->name);
    size_t b_length = strlen(b->name);
    size_t offset = 0;

    while (offset < a_length && offset < b_length &&
           a->name[offset] == b->name[offset]) {
        ++offset;
    }
    unsigned char a_next = offset < a_length
                               ? (unsigned char)a->name[offset]
                               : (unsigned char)(a->is_tree ? '/' : '\0');
    unsigned char b_next = offset < b_length
                               ? (unsigned char)b->name[offset]
                               : (unsigned char)(b->is_tree ? '/' : '\0');
    return (int)a_next - (int)b_next;
}

static int buffer_append(MgBuffer *buffer, const void *data, size_t length)
{
    if (length > SIZE_MAX - buffer->length) {
        return -1;
    }
    size_t required = buffer->length + length;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 128 : buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        unsigned char *grown = realloc(buffer->data, capacity);
        if (grown == NULL) {
            return -1;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    return 0;
}

static int write_tree(MgRepository *repository, MgTreeNode *node,
                      unsigned char oid[20])
{
    MgBuffer buffer = {0};
    int result = -1;

    for (size_t i = 0; i < node->count; ++i) {
        MgTreeEntry *entry = &node->entries[i];
        if (entry->is_tree &&
            write_tree(repository, entry->tree, entry->oid) != 0) {
            goto cleanup;
        }
    }
    if (node->count > 1) {
        qsort(node->entries, node->count, sizeof(*node->entries),
              tree_entry_compare);
    }

    for (size_t i = 0; i < node->count; ++i) {
        MgTreeEntry *entry = &node->entries[i];
        char mode[16];
        int mode_length = snprintf(
            mode, sizeof(mode), "%s",
            entry->is_tree ? "40000" : (entry->mode == 0100755U ? "100755" : "100644"));
        size_t name_length = strlen(entry->name);
        unsigned char nul = 0;
        if (mode_length < 0 ||
            buffer_append(&buffer, mode, (size_t)mode_length) != 0 ||
            buffer_append(&buffer, " ", 1) != 0 ||
            buffer_append(&buffer, entry->name, name_length) != 0 ||
            buffer_append(&buffer, &nul, 1) != 0 ||
            buffer_append(&buffer, entry->oid, 20) != 0) {
            goto cleanup;
        }
    }
    result = mg_write_object(repository, "tree", buffer.data,
                             buffer.length, oid);

cleanup:
    free(buffer.data);
    return result;
}

static void copy_identity(char *destination, size_t capacity,
                          const char *value, const char *fallback)
{
    if (value == NULL || value[0] == '\0') {
        value = fallback;
    }
    size_t length = strlen(value);
    if (length >= capacity) {
        length = capacity - 1;
    }
    for (size_t i = 0; i < length; ++i) {
        destination[i] = value[i] == '\n' || value[i] == '\r'
                             ? ' '
                             : value[i];
    }
    destination[length] = '\0';
}

static int read_head(const MgRepository *repository,
                     char ref_name[MG_PATH_MAX],
                     char ref_path[MG_PATH_MAX],
                     char parent_oid[41])
{
    char head_path[MG_PATH_MAX];
    char head[MG_PATH_MAX];
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
    parent_oid[0] = '\0';
    if (strncmp(head, "ref: ", 5) == 0) {
        const char *name = head + 5;
        if (strncmp(name, "refs/heads/", 11) != 0 ||
            strstr(name, "..") != NULL ||
            strlen(name) >= MG_PATH_MAX ||
            mg_path_join(ref_path, MG_PATH_MAX,
                         repository->git_dir, name) != 0) {
            return -1;
        }
        strcpy(ref_name, name);
        file = fopen(ref_path, "rb");
        if (file != NULL) {
            if (fgets(parent_oid, 41, file) == NULL) {
                fclose(file);
                return -1;
            }
            if (fclose(file) != 0) {
                return -1;
            }
            parent_oid[strcspn(parent_oid, "\r\n")] = '\0';
            if (strlen(parent_oid) != 40) {
                return -1;
            }
        }
        return 0;
    }
    if (strlen(head) != 40) {
        return -1;
    }
    strcpy(parent_oid, head);
    strcpy(ref_name, "HEAD");
    return mg_path_join(ref_path, MG_PATH_MAX,
                        repository->git_dir, "HEAD");
}

static int write_ref(const char *ref_path, const char *oid)
{
    char lock_path[MG_PATH_MAX];
    FILE *file;
    if (snprintf(lock_path, sizeof(lock_path), "%s.lock", ref_path) >=
        (int)sizeof(lock_path)) {
        return -1;
    }
    file = fopen(lock_path, "wbx");
    if (file == NULL) {
        return -1;
    }
    int result = fprintf(file, "%s\n", oid) < 0 ? -1 : 0;
    if (fclose(file) != 0) {
        result = -1;
    }
    if (result != 0) {
        remove(lock_path);
        return -1;
    }
#ifdef _WIN32
    if (!MoveFileExA(lock_path, ref_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(lock_path);
        return -1;
    }
#else
    if (rename(lock_path, ref_path) != 0) {
        remove(lock_path);
        return -1;
    }
#endif
    return 0;
}

static int parent_has_tree(const MgRepository *repository,
                           const char parent_oid[41],
                           const unsigned char tree_oid[20])
{
    unsigned char oid[20];
    unsigned char *data = NULL;
    size_t length = 0;
    char type[16];
    char tree_hex[41];
    int result = -1;

    for (size_t i = 0; i < 20; ++i) {
        int high_char = parent_oid[i * 2];
        int low_char = parent_oid[i * 2 + 1];
        int high = high_char >= '0' && high_char <= '9'
                       ? high_char - '0'
                       : (high_char | 32) - 'a' + 10;
        int low = low_char >= '0' && low_char <= '9'
                      ? low_char - '0'
                      : (low_char | 32) - 'a' + 10;
        if (high < 0 || high > 15 || low < 0 || low > 15) {
            return -1;
        }
        oid[i] = (unsigned char)((high << 4) | low);
    }
    if (mg_read_object(repository, oid, type, &data, &length) != 0 ||
        strcmp(type, "commit") != 0 || length < 46 ||
        memcmp(data, "tree ", 5) != 0 || data[45] != '\n') {
        free(data);
        return -1;
    }
    mg_oid_to_hex(tree_oid, tree_hex);
    result = memcmp(data + 5, tree_hex, 40) == 0;
    free(data);
    return result;
}

int mg_command_commit(const char *message)
{
    MgRepository repository;
    MgIndex index;
    MgTreeNode root = {0};
    unsigned char tree_oid[20];
    unsigned char commit_oid[20];
    char tree_hex[41];
    char commit_hex[41];
    char ref_name[MG_PATH_MAX];
    char ref_path[MG_PATH_MAX];
    char parent_oid[41];
    char author_name[256];
    char author_email[256];
    char identity[600];
    char commit_data[MG_PATH_MAX * 2];
    char timestamp[64];
    time_t now;
    int result = 1;

    if (message == NULL || message[0] == '\0') {
        fprintf(stderr, "mygit: commit message must not be empty\n");
        return 2;
    }
    mg_index_init(&index);
    if (mg_repository_find(&repository) != 0) {
        fprintf(stderr, "mygit: not a MiniGit repository (or any parent directory)\n");
        return 1;
    }
    if (mg_index_read(&repository, &index) != 0) {
        fprintf(stderr, "mygit: unable to read repository index\n");
        goto cleanup;
    }
    for (size_t i = 0; i < index.count; ++i) {
        if (tree_insert(&root, &index.entries[i]) != 0) {
            fprintf(stderr, "mygit: unable to create commit tree\n");
            goto cleanup;
        }
    }
    if (write_tree(&repository, &root, tree_oid) != 0 ||
        read_head(&repository, ref_name, ref_path, parent_oid) != 0) {
        fprintf(stderr, "mygit: unable to create commit tree or read HEAD\n");
        goto cleanup;
    }
    if (parent_oid[0] != '\0') {
        int has_changes = parent_has_tree(&repository, parent_oid, tree_oid);
        if (has_changes < 0) {
            fprintf(stderr, "mygit: unable to inspect parent commit\n");
            goto cleanup;
        }
        if (has_changes) {
            fprintf(stderr, "mygit: nothing to commit, working tree clean\n");
            goto cleanup;
        }
    } else if (index.count == 0) {
        fprintf(stderr, "mygit: nothing staged to commit\n");
        goto cleanup;
    }
    mg_oid_to_hex(tree_oid, tree_hex);

    copy_identity(author_name, sizeof(author_name),
                  getenv("GIT_AUTHOR_NAME"), "MiniGit User");
    copy_identity(author_email, sizeof(author_email),
                  getenv("GIT_AUTHOR_EMAIL"), "minigit@example.com");
    if (snprintf(identity, sizeof(identity), "%s <%s>",
                 author_name, author_email) >= (int)sizeof(identity)) {
        fprintf(stderr, "mygit: commit identity is too long\n");
        goto cleanup;
    }
    now = time(NULL);
    if (now == (time_t)-1 ||
        snprintf(timestamp, sizeof(timestamp), "%lld +0000",
                 (long long)now) >= (int)sizeof(timestamp)) {
        fprintf(stderr, "mygit: unable to determine commit time\n");
        goto cleanup;
    }
    int commit_length;
    if (parent_oid[0] != '\0') {
        commit_length = snprintf(commit_data, sizeof(commit_data),
                                 "tree %s\nparent %s\nauthor %s %s\n"
                                 "committer %s %s\n\n%s%s",
                                 tree_hex, parent_oid, identity, timestamp,
                                 identity, timestamp, message,
                                 message[strlen(message) - 1] == '\n' ? "" : "\n");
    } else {
        commit_length = snprintf(commit_data, sizeof(commit_data),
                                 "tree %s\nauthor %s %s\n"
                                 "committer %s %s\n\n%s%s",
                                 tree_hex, identity, timestamp,
                                 identity, timestamp, message,
                                 message[strlen(message) - 1] == '\n' ? "" : "\n");
    }
    if (commit_length < 0 || (size_t)commit_length >= sizeof(commit_data) ||
        mg_write_object(&repository, "commit", commit_data,
                        (size_t)commit_length, commit_oid) != 0) {
        fprintf(stderr, "mygit: unable to write commit object\n");
        goto cleanup;
    }
    mg_oid_to_hex(commit_oid, commit_hex);
    if (write_ref(ref_path, commit_hex) != 0) {
        fprintf(stderr, "mygit: unable to update '%s'\n", ref_name);
        goto cleanup;
    }

    printf("[%s %.7s] %s\n", ref_name, commit_hex, message);
    result = 0;

cleanup:
    tree_clear(&root);
    mg_index_clear(&index);
    return result;
}
