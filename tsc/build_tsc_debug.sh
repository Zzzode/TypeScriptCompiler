#!/bin/bash

set -ex

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

cmake --build "${ROOT_PATH}"/__build/tsc/debug --config Debug -j

bash -f "${ROOT_PATH}"/scripts/separate_debug_info.sh "${ROOT_PATH}"/__build/tsc/debug/bin/tsc
