#!/bin/bash

set -ex

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

if [[ ! -d ${ROOT_PATH}/__build/gc ]]; then
  mkdir -p "${ROOT_PATH}"/__build/gc
fi

if [[ ! -f "${ROOT_PATH}"/__build/gc/debug/compile_commands.json ]]; then
  cmake "${ROOT_PATH}"/3rdParty/gc-8.2.4 \
    -G "Ninja" \
    -B "${ROOT_PATH}"/__build/gc/debug \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="${ROOT_PATH}"/3rdParty/gc/debug \
    -Denable_threads=ON \
    -Denable_cplusplus=OFF
fi

if [[ ! -d "${ROOT_PATH}"/3rdParty/gc/debug ]]; then
  cmake --build "${ROOT_PATH}"/__build/gc/debug --config Debug -j

  mkdir -p "${ROOT_PATH}"/3rdParty/gc/debug

  cp "${ROOT_PATH}"/__build/gc/debug/lib* "${ROOT_PATH}"/3rdParty/gc/debug/
fi

echo "Build GC succeed..."
