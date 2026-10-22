#ifndef MYGIT_UTIL_REPOSITORY_H
#define MYGIT_UTIL_REPOSITORY_H

#include "util/filesystem.h"

typedef struct {
    char worktree[MG_PATH_MAX];
    char git_dir[MG_PATH_MAX];
} MgRepository;

int mg_repository_find(MgRepository *repository);
int mg_path_join(char *result, size_t result_size,
                 const char *parent, const char *child);

#endif
