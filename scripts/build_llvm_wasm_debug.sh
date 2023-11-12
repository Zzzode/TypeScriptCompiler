#!/bin/bash

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

cd "${ROOT_PATH}/__build/llvm/wasm/debug" || exit

cmake --build . --config Debug --target install -j

cmake --install . --config Debug
