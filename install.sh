#!/usr/bin/env bash

set -euo pipefail

if (($# > 1)); then
    echo "Usage: $0 [CMAKE_VARIABLE=VALUE]" >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=${SOAPYSIDEKIQ_BUILD_DIR:-"$repo_root/build/install-driver"}
cmake_args=(-U CMAKE_INSTALL_PREFIX -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
install_prefix=${SOAPYSIDEKIQ_INSTALL_PREFIX:-}

if [[ -n ${SIDEKIQ_SDK_DIR:-} ]]; then
    cmake_args+=("-DSIDEKIQ_SDK_DIR=$SIDEKIQ_SDK_DIR")
else
    cmake_args+=(-U SIDEKIQ_SDK_DIR)
fi
if [[ -n $install_prefix ]]; then
    cmake_args+=("-DCMAKE_INSTALL_PREFIX=$install_prefix")
fi
if (($# == 1)); then
    # Explicit arguments override environment defaults, after stale cache removal.
    if [[ $1 =~ ^CMAKE_INSTALL_PREFIX(:[^=]+)?=(.*)$ ]]; then
        install_prefix=${BASH_REMATCH[2]}
        if [[ -z $install_prefix ]]; then
            echo "CMAKE_INSTALL_PREFIX cannot be empty" >&2
            exit 2
        fi
    fi
    cmake_args+=("-D$1")
fi

cmake -S "$repo_root" -B "$build_dir" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel

if [[ -n $install_prefix ]]; then
    cmake --install "$build_dir"
else
    sudo cmake --install "$build_dir"
    sudo ldconfig
fi

