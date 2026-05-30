import subprocess
Import("env")

VERSION_STR = "v0.6.0"   # hand-bumped on release commits

try:
    sha = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=env["PROJECT_DIR"]
    ).decode().strip()
except Exception:
    sha = "unknown"

board_env = env.get("PIOENV", "unknown")

env.Append(CPPDEFINES=[
    ("FW_VERSION",     env.StringifyMacro(sha)),
    ("FW_VERSION_STR", env.StringifyMacro(VERSION_STR)),
    ("FW_BOARD_ENV",   env.StringifyMacro(board_env)),
])
print(f"fw_version: FW_VERSION_STR={VERSION_STR} FW_VERSION={sha} FW_BOARD_ENV={board_env}")
