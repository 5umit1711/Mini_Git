# MiniGit

MiniGit is a small, native C implementation of the core ideas behind Git. It
creates repositories using Git-compatible object and index formats, so the
standard Git CLI can inspect repositories and commits created by MiniGit.

The goal is to build a readable, modular version-control project—not to
reimplement every Git command or network protocol.

## Features

- Initialize a repository with a standard non-bare `.git` layout.
- Stage regular files and recursively stage directory contents.
- Stage tracked file removals with `add -A`.
- Show staged, unstaged, deleted, and untracked paths.
- Create commits with parent history and tree snapshots.
- Display commit history in full or one-line format.
- List and create local branches.
- Switch branches when tracked files and the index are clean.
- Preserve executable file modes on supported platforms.
- Read and write Git-compatible loose blob, tree, and commit objects.
- Read and write Git index version 2, including its checksum.
- Interoperate with Git for inspecting objects, trees, refs, status, and history.
- Run unit and workflow tests with CTest; run CI builds on Windows, Linux,
  and macOS.

## Requirements

- C17 compiler
- CMake 3.20 or newer
- zlib development headers and library
- Python 3 to enable the end-to-end CTest workflow test (optional)

## Build

From the repository root:

```sh
cmake -S . -B build
cmake --build build
```

On Windows PowerShell, run the executable as:

```powershell
.\build\mygit.exe --help
```

On Linux or macOS:

```sh
./build/mygit --help
```

If CMake cannot find zlib on Windows, install zlib with a package manager such
as vcpkg and configure CMake with that package manager's toolchain file.

## Quick start

Run these commands from the directory you want MiniGit to track:

```text
mygit init
mygit status
mygit add README.md src
mygit status
mygit commit -m "Initial project snapshot"
mygit log --oneline
```

To stage all current files and tracked removals:

```text
mygit add -A
mygit commit -m "Update project"
```

`add -A` stages all files in the repository, not just the current directory.
Review `status` before committing.

## Commands

| Command | Description |
| --- | --- |
| `mygit init [directory]` | Initialize a repository in the given directory, or the current directory if omitted. |
| `mygit add <path>...` | Stage one or more files or recursively stage directory contents. |
| `mygit add -A` / `mygit add --all` | Stage all current files and remove missing tracked files from the index. |
| `mygit status` | Show the current branch and staged, unstaged, deleted, and untracked changes. |
| `mygit commit -m "message"` | Create a commit from the current index and advance the current branch. |
| `mygit log` | Show commit history, including author and date. |
| `mygit log --oneline` | Show abbreviated commit IDs and one-line messages. |
| `mygit branch` | List local branches; `*` marks the current branch. |
| `mygit branch <name>` | Create a branch at the current commit. |
| `mygit switch <name>` | Switch to an existing branch if tracked files and the index are clean. |
| `mygit --help` | Show command usage. |
| `mygit --version` | Show the MiniGit version. |

### Commit identity

MiniGit uses `GIT_AUTHOR_NAME` and `GIT_AUTHOR_EMAIL` when set. Otherwise, it
uses `MiniGit User` and `minigit@example.com`. For example, in PowerShell:

```powershell
$env:GIT_AUTHOR_NAME = "Your Name"
$env:GIT_AUTHOR_EMAIL = "you@example.com"
```

## Architecture

The project is split by responsibility:

- `src/main.c` — global options, command parsing, and dispatch.
- `src/commands/` — implementations for `init`, `add`, `status`, `commit`,
  `log`, and `branch` operations.
- `src/util/sha1.c` — streaming SHA-1 implementation used for object IDs and
  checksums.
- `src/util/object.c` — Git-style object hashing, zlib compression, storage,
  and reading.
- `src/util/index.c` — Git index version 2 parsing and writing.
- `src/util/repository.c` — repository discovery and path utilities.
- `src/util/filesystem.c` — portable filesystem helpers.
- `tests/` — SHA-1 unit tests and end-to-end command workflow tests.

Git objects are stored in `.git/objects` using Git's loose-object format:
the object type and content length are hashed with SHA-1, and the serialized
object is compressed with zlib. Commits reference trees and parent commits;
trees contain file modes, names, and blob IDs. The index stores the staged
snapshot separately from the current commit, as in Git's object/index model.

## Testing

Configure and build, then run:

```sh
ctest --test-dir build --output-on-failure
```

The SHA-1 test covers known vectors and streaming input. When Python 3 is
available, the workflow test exercises initialization, staging, commits,
branching, safe switching, deletion staging, and Git interoperability.

## Current limitations

MiniGit is intentionally a subset of Git. It does **not** currently provide:

- Remotes, clone, fetch, push, or network protocols.
- Merge, rebase, tags, stash, cherry-pick, or conflict resolution.
- Diff or checkout by commit.
- `.gitignore` processing or Git's complete pathspec syntax.
- Rename detection or symlink/submodule support.
- The full Git configuration, hooks, and index extensions.
- Full compatibility with every Git repository feature or command option.

Branch switching refuses to proceed if the index differs from the current
commit or tracked working files have local changes. It also refuses to
overwrite a colliding untracked file. These safeguards are deliberate: the
current implementation does not yet have a merge/conflict workflow.

## Contributing

Keep command implementations in `src/commands/` and reusable repository
primitives in `src/util/`. Build with warnings enabled and run the CTest suite
before submitting changes. The GitHub Actions workflow builds and tests on
Windows, Linux, and macOS.
