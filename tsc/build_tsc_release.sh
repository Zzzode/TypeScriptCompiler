#!/bin/sh

set -ex

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

cd "${ROOT_PATH}"/__build/tsc/ninja/release
cmake --build . --config Release -j 8
