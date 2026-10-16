#!/usr/bin/env bash

# Compatibility entry point; CMake now produces the compile database directly.
set -euo pipefail
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec "$repo_root/install.sh" "$@"
