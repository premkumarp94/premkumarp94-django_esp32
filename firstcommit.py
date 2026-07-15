from pathlib import Path
from datetime import datetime
import subprocess
import shutil
import sys

# ==========================================
# Configuration
# ==========================================

REMOTE_URL = "https://github.com/premkumarp94/premkumarp94-django_esp32.git"

PREFIX = Path.cwd().name.upper()

COMMIT_MESSAGE = (
    f"{PREFIX}_"
    f"{datetime.now():%Y_%m_%d_%H_%M_%S}"
)

# ==========================================

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


# Remove existing git repository if present
if Path(".git").exists():
    print("ERROR: This folder already contains a Git repository.")
    print("Delete the .git folder manually and run again.")
    sys.exit(1)

# Initialize git
run(["git", "init"])

# Create main branch
run(["git", "branch", "-M", "main"])

# Add remote
run(["git", "remote", "add", "origin", REMOTE_URL])

# Add files
run(["git", "add", "."])

# Commit
run(["git", "commit", "-m", COMMIT_MESSAGE])

# First push
run(["git", "push", "-u", "origin", "main"])

print("\n========================================")
print("Repository initialized successfully!")
print("Project :", PREFIX)
print("Commit  :", COMMIT_MESSAGE)
print("========================================")