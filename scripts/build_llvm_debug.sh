#!/bin/bash

set -e

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

if [[ ! -d "${ROOT_PATH}"/__build/llvm/debug ]]; then
  echo "${ROOT_PATH}/__build/llvm/debug not existed, please run config_llvm_debug.sh first."
  exit 1
fi

if [[ ! -d "${ROOT_PATH}"/3rdParty/llvm/debug/bin ]]; then
  cmake --build "${ROOT_PATH}"/__build/llvm/debug --config Debug --target install -j
  cmake --install "${ROOT_PATH}"/__build/llvm/debug --config Debug
fi

echo "Build LLVM Debug succeed..."
