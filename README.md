# MiniGit

MiniGit is a native C implementation of a growing subset of Git. It currently
provides:

- `mygit init [directory]` to create a repository.
- `mygit add <pathspec>...` to stage regular files and directory contents;
  `mygit add -A` also stages tracked-file deletions.
- `mygit status` to compare the index, working tree, and current commit.
- `mygit commit -m <message>` to create a snapshot and advance the current
  branch.
- `mygit log [--oneline]` to inspect first-parent history.
- `mygit branch [name]` to list or create a branch.
- `mygit switch <name>` to switch branches when the index and tracked
  worktree are clean.

## Build

Requirements: a C17 compiler, CMake 3.20 or newer, and zlib development
headers/library. Python 3 is optional and enables the end-to-end CTest.

Configure and build with CMake:

```text
cmake -S . -B build
cmake --build build
```

The resulting executable is `mygit` (or `mygit.exe` on Windows).

## Quick start

```text
mygit init
mygit add -A
mygit commit -m "Initial snapshot"
mygit branch feature
mygit switch feature
mygit log --oneline
```

## Architecture

- `src/main.c` handles global options and dispatches commands.
- `src/commands/` contains command implementations.
- `src/util/` contains the repository index, SHA-1 object storage,
  repository discovery, and reusable filesystem helpers.

The repository format is initialized with Git's standard `HEAD`, `config`,
`description`, `hooks`, `info`, `objects`, and `refs` paths, including
`info/exclude`. Blob, tree, and commit objects use Git's zlib-compressed object
format; the index is written in Git index version 2 format. Git can read
MiniGit's staged objects, trees, commits, and staged deletions.

## Tests

Build and run the focused CTest suite:

```text
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The CTest suite covers SHA-1 and an end-to-end repository workflow (when
Python 3 is available). Branch switching intentionally refuses local tracked
changes and refuses to overwrite colliding untracked files. `mygit add -A`
stages tracked deletions. This is not full Git parity: checkout by commit,
diff, merge, remote protocols, ignore rules, renames, hooks, and many other
commands are not implemented yet.
