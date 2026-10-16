#!/usr/bin/env bash

set -euo pipefail

if (($# > 1)); then
    echo "Usage: $0 [CMAKE_VARIABLE=VALUE]" >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
build_dir=${SOAPYSIDEKIQ_BUILD_DIR:-"$repo_root/build/install-driver"}
cmake_args=(-U Sidekiq_CONFIG_EXECUTABLE)

if [[ -n ${SIDEKIQ_SDK_DIR:-} ]]; then
    cmake_args+=("-DSIDEKIQ_SDK_DIR=$SIDEKIQ_SDK_DIR")
else
    cmake_args+=(-U SIDEKIQ_SDK_DIR)
fi
if (($# == 1)); then
    cmake_args+=("-D$1")
fi
if [[ -n ${SOAPYSIDEKIQ_INSTALL_PREFIX:-} ]]; then
    cmake_args+=("-DCMAKE_INSTALL_PREFIX=$SOAPYSIDEKIQ_INSTALL_PREFIX")
else
    # Do not reuse an isolated prefix cached by an earlier run.
    cmake_args+=(-U CMAKE_INSTALL_PREFIX)
fi

cmake -S "$repo_root" -B "$build_dir" "${cmake_args[@]}"
cmake --build "$build_dir" --parallel

if [[ -n ${SOAPYSIDEKIQ_INSTALL_PREFIX:-} ]]; then
    cmake --install "$build_dir"
else
    sudo cmake --install "$build_dir"
    sudo ldconfig
fi

