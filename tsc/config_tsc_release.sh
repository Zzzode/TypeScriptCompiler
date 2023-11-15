#!/bin/sh

set -ex

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

mkdir -p "${ROOT_PATH}"/__build/tsc/ninja/release
cd "${ROOT_PATH}"/__build/tsc/ninja/release
cmake "${ROOT_PATH}"/tsc -G "Ninja" -DCMAKE_BUILD_TYPE=Release -Wno-dev

