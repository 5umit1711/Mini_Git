#include "commands/init.h"

#include <stdio.h>
#include <string.h>

#include "util/filesystem.h"

static int join_path(char *result, size_t result_size,
                     const char *directory, const char *child)
{
    size_t directory_length = strlen(directory);
    const char *separator = "";

    if (directory_length > 0 &&
        directory[directory_length - 1] != '/' &&
        directory[directory_length - 1] != '\\') {
        separator = MG_PATH_SEPARATOR;
    }

    if (snprintf(result, result_size, "%s%s%s",
                 directory, separator, child) >= (int)result_size) {
        fprintf(stderr, "mygit: repository path is too long\n");
        return -1;
    }

    return 0;
}

static int create_repository_file(const char *git_directory,
                                  const char *name, const char *contents)
{
    char path[MG_PATH_MAX];
    int result = mg_write_file_if_missing(git_directory, name, contents);

    if (result >= 0) {
        return 0;
    }

    if (join_path(path, sizeof(path), git_directory, name) == 0) {
        fprintf(stderr, "mygit: unable to create '%s'\n", path);
    }
    return -1;
}

int mg_command_init(const char *directory)
{
    char git_directory[MG_PATH_MAX];
    char info_directory[MG_PATH_MAX];
    int already_initialized;
#ifdef _WIN32
    const char *config =
        "[core]\n"
        "\trepositoryformatversion = 0\n"
        "\tfilemode = false\n"
        "\tbare = false\n"
        "\tlogallrefupdates = true\n";
#else
    const char *config =
        "[core]\n"
        "\trepositoryformatversion = 0\n"
        "\tfilemode = true\n"
        "\tbare = false\n"
        "\tlogallrefupdates = true\n";
#endif

    if (mg_ensure_directory(directory) != 0) {
        fprintf(stderr, "mygit: unable to create or access directory '%s'\n",
                directory);
        return 1;
    }

    if (join_path(git_directory, sizeof(git_directory), directory, ".git") != 0) {
        return 1;
    }

    already_initialized = mg_is_directory(git_directory);
    if (mg_ensure_directory(git_directory) != 0) {
        fprintf(stderr, "mygit: unable to create repository directory '%s'\n",
                git_directory);
        return 1;
    }

    if (create_repository_file(git_directory, "HEAD",
                               "ref: refs/heads/master\n") != 0 ||
        create_repository_file(git_directory, "config",
                               config) != 0 ||
        create_repository_file(git_directory, "description",
                               "Unnamed repository; edit this file 'description' to name the repository.\n") != 0 ||
        mg_ensure_subdirectory(git_directory, "objects") != 0 ||
        mg_ensure_nested_directory(git_directory, "objects", "info") != 0 ||
        mg_ensure_nested_directory(git_directory, "objects", "pack") != 0 ||
        mg_ensure_subdirectory(git_directory, "refs") != 0 ||
        mg_ensure_nested_directory(git_directory, "refs", "heads") != 0 ||
        mg_ensure_nested_directory(git_directory, "refs", "tags") != 0 ||
        mg_ensure_subdirectory(git_directory, "hooks") != 0 ||
        mg_ensure_subdirectory(git_directory, "info") != 0) {
        fprintf(stderr, "mygit: unable to create repository structure in '%s'\n",
                git_directory);
        return 1;
    }

    if (join_path(info_directory, sizeof(info_directory),
                  git_directory, "info") != 0 ||
        create_repository_file(info_directory, "exclude",
                               "# git ls-files --others --exclude-from=.git/info/exclude\n"
                               "# Lines that start with '#' are comments.\n") != 0) {
        return 1;
    }

    printf("%s repository in %s\n",
           already_initialized ? "Reinitialized existing MiniGit" :
                                 "Initialized empty MiniGit",
           git_directory);
    return 0;
}
