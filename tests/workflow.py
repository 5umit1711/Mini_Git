import pathlib
import shutil
import subprocess
import sys
import tempfile


def run(executable, *arguments, cwd, expected=0):
    result = subprocess.run(
        [executable, *arguments],
        cwd=cwd,
        text=True,
        capture_output=True,
        check=False,
    )
    if result.returncode != expected:
        raise AssertionError(
            f"mygit {' '.join(arguments)} returned {result.returncode}, "
            f"expected {expected}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def main():
    executable = str(pathlib.Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mygit-workflow-") as temporary:
        repository = pathlib.Path(temporary) / "repository"
        repository.mkdir()
        (repository / "nested").mkdir()
        (repository / "nested" / "base.txt").write_text("base\n")
        (repository / "removed.txt").write_text("remove me\n")

        run(executable, "init", str(repository), cwd=temporary)
        run(executable, "add", "-A", cwd=repository)
        run(executable, "commit", "-m", "Initial snapshot", cwd=repository)
        run(executable, "branch", "feature", cwd=repository)
        run(executable, "switch", "feature", cwd=repository)

        (repository / "nested" / "base.txt").write_text("feature version\n")
        (repository / "feature.txt").write_text("feature-only file\n")
        run(executable, "add", "-A", cwd=repository)
        run(executable, "commit", "-m", "Feature work", cwd=repository)
        run(executable, "switch", "master", cwd=repository)
        assert (repository / "nested" / "base.txt").read_text() == "base\n"
        assert not (repository / "feature.txt").exists()

        (repository / "removed.txt").unlink()
        run(executable, "add", "-A", cwd=repository)
        status = run(executable, "status", cwd=repository)
        assert "deleted:    removed.txt" in status.stdout
        (repository / "nested" / "base.txt").unlink()
        run(executable, "add", "-A", cwd=repository)
        run(executable, "commit", "-m", "Remove tracked file", cwd=repository)
        empty_tree = subprocess.run(
            [shutil.which("git") or "git", "ls-tree", "-r", "--name-only", "HEAD"],
            cwd=repository,
            text=True,
            capture_output=True,
            check=False,
        ) if shutil.which("git") is not None else None
        if empty_tree is not None:
            assert empty_tree.returncode == 0
            assert empty_tree.stdout == ""

        (repository / "feature.txt").write_text("untracked collision\n")
        collision = run(executable, "switch", "feature", cwd=repository, expected=1)
        assert "overwrite untracked file" in collision.stderr
        (repository / "feature.txt").unlink()
        run(executable, "switch", "feature", cwd=repository)
        assert (repository / "removed.txt").read_text() == "remove me\n"
        (repository / "nested" / "base.txt").write_text("local edit\n")
        rejected_switch = run(
            executable, "switch", "master", cwd=repository, expected=1
        )
        assert "commit or discard local changes" in rejected_switch.stderr

        branches = run(executable, "branch", cwd=repository)
        assert "* feature" in branches.stdout
        assert "  master" in branches.stdout

        git = shutil.which("git")
        if git is not None:
            interoperable = subprocess.run(
                [git, "status", "--short"],
                cwd=repository,
                text=True,
                capture_output=True,
                check=False,
            )
            if interoperable.returncode != 0:
                raise AssertionError(
                    f"Git rejected the repository:\n{interoperable.stderr}"
                )
            history = subprocess.run(
                [git, "log", "--all", "--oneline"],
                cwd=repository,
                text=True,
                capture_output=True,
                check=False,
            )
            if history.returncode != 0 or "Feature work" not in history.stdout:
                raise AssertionError("Git could not read MiniGit commit history")


if __name__ == "__main__":
    main()
