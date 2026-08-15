#!/usr/bin/env bash
# Shared entry point for the POSIX host-tool wrappers.
#
# Keep this file intentionally small and dependency-free: it is sourced by the
# wrappers from Linux, macOS, and Git Bash.  The Python tool remains the source
# of truth for command behavior and reads TRIT_BUILD_DIR directly from the
# inherited environment when a build directory is not supplied explicitly.

set -euo pipefail

trit_posix_script_dir=$(
    CDPATH= cd -P "$(dirname "${BASH_SOURCE[0]}")" >/dev/null
    pwd -P
)
trit_posix_repo_root=$(CDPATH= cd -P "$trit_posix_script_dir/.." >/dev/null && pwd -P)
trit_posix_tool="$trit_posix_repo_root/tools/trit_tool.py"

if [[ ! -f "$trit_posix_tool" ]]; then
    printf 'trit: cannot find host tool: %s\n' "$trit_posix_tool" >&2
    return 1 2>/dev/null || exit 1
fi

if [[ -n "${TRIT_PYTHON:-}" ]]; then
    trit_posix_python=$TRIT_PYTHON
elif command -v python3 >/dev/null 2>&1; then
    trit_posix_python=$(command -v python3)
elif command -v python >/dev/null 2>&1; then
    trit_posix_python=$(command -v python)
else
    printf 'trit: Python 3 is required (set TRIT_PYTHON to its executable)\n' >&2
    return 127 2>/dev/null || exit 127
fi

if [[ "$trit_posix_python" == */* && ! -x "$trit_posix_python" ]]; then
    printf 'trit: Python executable is not executable: %s\n' "$trit_posix_python" >&2
    return 126 2>/dev/null || exit 126
fi

trit_posix_run() {
    if [[ $# -lt 1 ]]; then
        printf 'trit: internal wrapper error: missing command\n' >&2
        return 2
    fi
    local trit_posix_command=$1
    shift
    # Do not cd or re-tokenize arguments here.  trit_tool.py resolves the
    # repository itself, while this exec preserves every caller-supplied argv.
    exec "$trit_posix_python" "$trit_posix_tool" "$trit_posix_command" "$@"
}
