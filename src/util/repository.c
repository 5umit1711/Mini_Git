#include "util/repository.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define mg_getcwd(buffer, size) _getcwd(buffer, (int)(size))
#else
#include <unistd.h>
#define mg_getcwd(buffer, size) getcwd(buffer, size)
#endif

int mg_path_join(char *result, size_t result_size,
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
        return -1;
    }
    return 0;
}

static int has_git_directory(const char *path)
{
    char git_path[MG_PATH_MAX];
    return mg_path_join(git_path, sizeof(git_path), path, ".git") == 0 &&
           mg_is_directory(git_path);
}

int mg_repository_find(MgRepository *repository)
{
    size_t length;

    if (mg_getcwd(repository->worktree, sizeof(repository->worktree)) == NULL) {
        return -1;
    }

    for (;;) {
        if (has_git_directory(repository->worktree)) {
            return mg_path_join(repository->git_dir,
                                sizeof(repository->git_dir),
                                repository->worktree, ".git");
        }

        length = strlen(repository->worktree);
        while (length > 1 &&
               (repository->worktree[length - 1] == '/' ||
                repository->worktree[length - 1] == '\\')) {
            repository->worktree[--length] = '\0';
        }
        while (length > 0 &&
               repository->worktree[length - 1] != '/' &&
               repository->worktree[length - 1] != '\\') {
            repository->worktree[--length] = '\0';
        }
#ifdef _WIN32
        if (length == 3 && repository->worktree[1] == ':') {
            return -1;
        }
#endif
        if (length <= 1) {
            return -1;
        }
        repository->worktree[length - 1] = '\0';
    }
}
