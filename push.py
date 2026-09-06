from pathlib import Path
from datetime import datetime
import subprocess
import sys

PREFIX = Path.cwd().name.upper()

COMMIT_MESSAGE = (
    f"{PREFIX}_"
    f"{datetime.now():%Y_%m_%d_%H_%M_%S}"
)

arg = sys.argv[1].lower() if len(sys.argv) > 1 else ""
FORCE_DATA_IGNORED = (arg == "force")
FORCE_EVERYTHING = (arg == "force2")
IS_FORCE = FORCE_DATA_IGNORED or FORCE_EVERYTHING


def run(cmd):
    print("> " + " ".join(cmd))
    result = subprocess.run(cmd, text=True, capture_output=True)
    if result.stdout:
        print(result.stdout.strip())
    if result.stderr:
        print(result.stderr.strip())
    if result.returncode != 0:
        sys.exit(result.returncode)


if FORCE_DATA_IGNORED:
    print("Mode: FORCE (Ignoring database/data files)...")
    # Stage all files except database files
    run(["git", "add", "--all", "--", ":!db.sqlite3", ":!*.sqlite3", ":!*.db"])
    # Ensure any previously staged db files are unstaged
    subprocess.run(["git", "reset", "HEAD", "--", "db.sqlite3", "*.sqlite3", "*.db"], capture_output=True)
else:
    if FORCE_EVERYTHING:
        print("Mode: FORCE2 (Forcing everything including database/data files)...")
    else:
        print("Mode: Standard Push...")
    run(["git", "add", "."])

# Check if anything is staged for commit
status = subprocess.run(
    ["git", "diff", "--cached", "--name-only"],
    capture_output=True,
    text=True
)

if status.stdout.strip():
    run([
        "git",
        "commit",
        "-m",
        COMMIT_MESSAGE
    ])
else:
    print("Nothing new staged to commit.")

if IS_FORCE:
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

print("\nDone.")
