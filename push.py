from pathlib import Path
from datetime import datetime
import subprocess
import sys

PREFIX = Path.cwd().name.upper()

COMMIT_MESSAGE = (
    f"{PREFIX}_"
    f"{datetime.now():%Y_%m_%d_%H_%M_%S}"
)

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


status = subprocess.run(
    ["git", "status", "--porcelain"],
    capture_output=True,
    text=True
)

if status.stdout.strip():

    run(["git", "add", "."])

    run([
        "git",
        "commit",
        "-m",
        COMMIT_MESSAGE
    ])

else:

    print("Nothing to commit.")
if FORCE:
    run([
        "git",
        "push",
        "--force"
    ])
else:
    run([
        "git",
        "push"
    ])
print("\nDone .")
