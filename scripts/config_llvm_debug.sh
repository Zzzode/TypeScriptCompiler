#!/bin/bash

set -ex

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

if [[ ! -d "${ROOT_PATH}"/__build/llvm ]]; then
  mkdir -p "${ROOT_PATH}"/__build/llvm
fi

if [[ ! -f "${ROOT_PATH}"/__build/llvm/debug/compile_commands.json ]]; then
  cmake "${ROOT_PATH}"/3rdParty/llvm-project/llvm \
    -G "Ninja" \
    -B "${ROOT_PATH}"/__build/llvm/debug \
    -DLLVM_TARGETS_TO_BUILD="host;AArch64" \
    -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="WebAssembly" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_INSTALL_PREFIX="${ROOT_PATH}"/3rdParty/llvm/debug \
    -DLLVM_INSTALL_UTILS=ON \
    -DLLVM_ENABLE_ASSERTIONS=ON \
    -DLLVM_ENABLE_PLUGINS=ON \
    -DLLVM_ENABLE_PROJECTS="clang;lld;mlir" \
    -DLLVM_ENABLE_EH=ON \
    -DLLVM_ENABLE_RTTI=ON \
    -DLLVM_REQUIRES_RTTI=ON \
    -DLLVM_ENABLE_PIC=ON
fi

echo "Config LLVM Debug succeed..."
