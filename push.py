import subprocess
import sys


def run(command):
    print("> " + " ".join(command))

    result = subprocess.run(
        command,
        text=True,
        capture_output=True
    )

    if result.stdout:
        print(result.stdout.strip())

    if result.stderr:
        print(result.stderr.strip())

    if result.returncode != 0:
        sys.exit(result.returncode)


run(["git", "push"])

print("\nPush completed successfully.")
