#!/usr/bin/env bash
set -euo pipefail
trit_posix_script_dir=$(CDPATH= cd -P "$(dirname "${BASH_SOURCE[0]}")" >/dev/null && pwd -P)
# shellcheck source=tools/trit-posix.sh
. "$trit_posix_script_dir/trit-posix.sh"
trit_posix_run compact-disk "$@"
