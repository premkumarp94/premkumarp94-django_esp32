import subprocess
import sys

FORCE = (
    len(sys.argv) > 1 and
    sys.argv[1].lower() == "force"
)


def run(cmd):
    print("> " + " ".join(cmd))

    result = subprocess.run(
        cmd,
        text=True,
        capture_output=True
    )

    if result.stdout:
        print(result.stdout.strip())

    if result.stderr:
        print(result.stderr.strip())

    if result.returncode != 0:
        sys.exit(result.returncode)


if FORCE:

    print("Resetting local repository...")

    run(["git", "fetch"])

    run([
        "git",
        "reset",
        "--hard",
        "origin/main"
    ])

    run([
        "git",
        "clean",
        "-fd"
    ])

else:

    run([
        "git",
        "pull"
    ])

print("\nDone.")