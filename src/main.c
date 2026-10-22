#include <stdio.h>
#include <string.h>

#include "commands/add.h"
#include "commands/branch.h"
#include "commands/commit.h"
#include "commands/init.h"
#include "commands/log.h"
#include "commands/status.h"

#define MYGIT_VERSION "0.1.0"

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage: mygit <command> [options]\n"
            "\n"
            "Commands:\n"
            "  init [directory]   Create an empty repository\n"
            "  add <pathspec>...  Stage paths; use -A to stage deletions too\n"
            "  branch [name]      List branches or create one\n"
            "  switch <name>      Switch branches with a clean worktree\n"
            "  commit -m <message> Record the staged snapshot\n"
            "  log [--oneline]    Show commit history\n"
            "  status             Show working tree status\n"
            "\n"
            "Options:\n"
            "  -h, --help         Show this help message\n"
            "  --version          Show version information\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage(stdout);
        return 0;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("mygit version %s\n", MYGIT_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        if (argc > 3) {
            fprintf(stderr, "mygit: 'init' accepts at most one directory\n");
            return 2;
        }
        return mg_command_init(argc == 3 ? argv[2] : ".");
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "mygit: 'add' requires a pathspec or -A\n");
            return 2;
        }
        if ((strcmp(argv[2], "-A") == 0 || strcmp(argv[2], "--all") == 0) &&
            argc != 3) {
            fprintf(stderr, "mygit: 'add -A' cannot be combined with pathspecs\n");
            return 2;
        }
        return mg_command_add(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "branch") == 0) {
        return mg_command_branch(argc - 2, argv + 2);
    }

    if (strcmp(argv[1], "switch") == 0) {
        if (argc != 3) {
            fprintf(stderr, "mygit: usage: mygit switch <name>\n");
            return 2;
        }
        char *arguments[] = {"--switch", argv[2]};
        return mg_command_branch(2, arguments);
    }

    if (strcmp(argv[1], "status") == 0) {
        if (argc != 2) {
            fprintf(stderr, "mygit: 'status' does not accept arguments yet\n");
            return 2;
        }
        return mg_command_status();
    }

    if (strcmp(argv[1], "commit") == 0) {
        if (argc != 4 || strcmp(argv[2], "-m") != 0) {
            fprintf(stderr, "mygit: usage: mygit commit -m <message>\n");
            return 2;
        }
        return mg_command_commit(argv[3]);
    }

    if (strcmp(argv[1], "log") == 0) {
        if (argc > 3 || (argc == 3 && strcmp(argv[2], "--oneline") != 0)) {
            fprintf(stderr, "mygit: usage: mygit log [--oneline]\n");
            return 2;
        }
        return mg_command_log(argc == 3);
    }

    fprintf(stderr, "mygit: unknown command '%s'\n", argv[1]);
    print_usage(stderr);
    return 2;
}