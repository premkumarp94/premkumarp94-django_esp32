import subprocess
import sys
import shutil
from pathlib import Path

arg = sys.argv[1].lower() if len(sys.argv) > 1 else ""
FORCE_DATA_IGNORED = (arg == "force")
FORCE_EVERYTHING = (arg == "force2")


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
    print("Mode: FORCE (Resetting code while PRESERVING local database/data)...")

    # Backup local sqlite database files temporarily if they exist
    db_backups = {}
    for db_path in Path(".").glob("*.sqlite3"):
        bak_file = db_path.with_name(f"{db_path.name}.local_bak")
        shutil.copy2(db_path, bak_file)
        db_backups[db_path] = bak_file

    run(["git", "fetch", "origin"])
    run(["git", "reset", "--hard", "origin/main"])
    run(["git", "clean", "-fd", "-e", "*.local_bak"])

    # Restore local database
    for original_path, bak_file in db_backups.items():
        if bak_file.exists():
            shutil.copy2(bak_file, original_path)
            bak_file.unlink()
            print(f"Preserved and restored local database: {original_path.name}")

elif FORCE_EVERYTHING:
    print("Mode: FORCE2 (Force reset EVERYTHING including overwriting database)...")
    run(["git", "fetch", "origin"])
    run(["git", "reset", "--hard", "origin/main"])
    run(["git", "clean", "-fd"])

else:
    print("Mode: Standard Pull...")
    run(["git", "pull"])

print("\nDone.")