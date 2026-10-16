#!/usr/bin/env bash

# Keep a per-run history while out.txt follows the current test.
set -uo pipefail

if (($# < 2)); then
    echo "Usage: $0 LABEL COMMAND [ARGS...]" >&2
    exit 2
fi

label=$1
shift
if [[ ! $label =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "LABEL may contain only letters, digits, dots, underscores, and dashes" >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
logs_dir="$repo_root/logs"
mkdir -p -- "$logs_dir"

if [[ -e "$repo_root/out.txt" && ! -L "$repo_root/out.txt" ]]; then
    echo "out.txt is a regular file; move it into logs/ before running" >&2
    exit 1
fi

stamp=$(date -u +%Y%m%dT%H%M%SZ)
log=$(mktemp "$logs_dir/${stamp}_${label}_XXXXXX.log")
current_log="$logs_dir/current.log"
{
    printf 'UTC start: %s\n' "$(date -u --iso-8601=seconds)"
    printf 'Git commit: %s\n' "$(git -C "$repo_root" rev-parse HEAD)"
    printf 'Command:'
    printf ' %q' "$@"
    printf '\n\n'
} > "$log"

if [[ ! -L "$repo_root/out.txt" ]]; then
    ln -s logs/current.log "$repo_root/out.txt"
elif [[ $(readlink "$repo_root/out.txt") != logs/current.log ]]; then
    ln -sfn logs/current.log "$repo_root/out.txt"
fi
cat -- "$log" >> "$current_log"

"$@" 2>&1 | tee -a "$log" "$current_log"
result=${PIPESTATUS[0]}
printf '\nExit status: %d\n' "$result" | tee -a "$log" "$current_log"
exit "$result"
