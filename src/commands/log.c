#include "commands/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/object.h"
#include "util/repository.h"

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

static int read_head_oid(const MgRepository *repository, char oid[41])
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
    if (strncmp(head, "ref: ", 5) == 0) {
        const char *ref = head + 5;
        if (strncmp(ref, "refs/heads/", 11) != 0 ||
            strstr(ref, "..") != NULL ||
            mg_path_join(ref_path, sizeof(ref_path),
                         repository->git_dir, ref) != 0) {
            return -1;
        }
        file = fopen(ref_path, "rb");
        if (file == NULL) {
            return errno == ENOENT ? 1 : -1;
        }
        if (fgets(oid, 41, file) == NULL) {
            fclose(file);
            return -1;
        }
        if (fclose(file) != 0) {
            return -1;
        }
        oid[strcspn(oid, "\r\n")] = '\0';
    } else {
        if (strlen(head) != 40) {
            return -1;
        }
        strcpy(oid, head);
    }
    return parse_oid(oid, (unsigned char[20]){0});
}

static void print_commit(const char *oid_hex, const unsigned char *data,
                         size_t length, int oneline, char parent[41])
{
    size_t offset = 0;
    size_t message_offset = length;
    char author[512] = "unknown";
    char date[64] = "unknown";
    parent[0] = '\0';

    while (offset < length) {
        size_t line_length = 0;
        while (offset + line_length < length &&
               data[offset + line_length] != '\n') {
            ++line_length;
        }
        if (line_length == 0) {
            message_offset = offset + 1;
            break;
        }
        if (line_length > 7 && memcmp(data + offset, "parent ", 7) == 0 &&
            parent[0] == '\0') {
            if (line_length == 47) {
                memcpy(parent, data + offset + 7, 40);
                parent[40] = '\0';
            }
        } else if (line_length > 7 &&
                   memcmp(data + offset, "author ", 7) == 0) {
            const char *identity = (const char *)data + offset + 7;
            size_t identity_length = line_length - 7;
            if (identity_length < sizeof(author)) {
                memcpy(author, identity, identity_length);
                author[identity_length] = '\0';
                const char *date_start = strrchr(author, '>');
                if (date_start != NULL && date_start[1] == ' ') {
                    snprintf(date, sizeof(date), "%s", date_start + 2);
                    ((char *)date_start)[1] = '\0';
                }
            }
        }
        offset += line_length + 1;
    }

    if (oneline) {
        size_t first_line = message_offset;
        while (first_line < length && data[first_line] != '\n') {
            ++first_line;
        }
        printf("%.7s ", oid_hex);
        if (first_line > message_offset) {
            fwrite(data + message_offset, 1, first_line - message_offset, stdout);
        }
        putchar('\n');
        return;
    }

    printf("commit %s\nAuthor: %s\nDate:   %s\n\n", oid_hex, author, date);
    while (message_offset < length) {
        size_t line_length = 0;
        while (message_offset + line_length < length &&
               data[message_offset + line_length] != '\n') {
            ++line_length;
        }
        printf("    ");
        if (line_length > 0) {
            fwrite(data + message_offset, 1, line_length, stdout);
        }
        putchar('\n');
        message_offset += line_length + 1;
    }
    putchar('\n');
}

int mg_command_log(int oneline)
{
    MgRepository repository;
    char oid_hex[41];

    if (mg_repository_find(&repository) != 0) {
        fprintf(stderr, "mygit: not a MiniGit repository (or any parent directory)\n");
        return 1;
    }
    int head_result = read_head_oid(&repository, oid_hex);
    if (head_result == 1) {
        return 0;
    }
    if (head_result != 0) {
        fprintf(stderr, "mygit: unable to read HEAD\n");
        return 1;
    }

    for (;;) {
        unsigned char oid[20];
        unsigned char *data = NULL;
        size_t length = 0;
        char type[16];
        char parent[41];

        if (parse_oid(oid_hex, oid) != 0 ||
            mg_read_object(&repository, oid, type, &data, &length) != 0 ||
            strcmp(type, "commit") != 0) {
            free(data);
            fprintf(stderr, "mygit: invalid commit object in history\n");
            return 1;
        }
        print_commit(oid_hex, data, length, oneline, parent);
        free(data);
        if (parent[0] == '\0') {
            break;
        }
        if (parse_oid(parent, oid) != 0) {
            fprintf(stderr, "mygit: invalid parent object id in history\n");
            return 1;
        }
        strcpy(oid_hex, parent);
    }
    return 0;
}
