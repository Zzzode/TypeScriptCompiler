#!/bin/bash

set -e

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

cmake "${ROOT_PATH}"/tsc -G "Ninja" -B "${ROOT_PATH}"/__build/tsc/release -DCMAKE_BUILD_TYPE=Release -Wno-dev

