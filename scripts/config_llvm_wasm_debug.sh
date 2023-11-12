#!/bin/bash

SCRIPT=$(readlink -f "$0")
SCRIPT_PATH=$(dirname "${SCRIPT}")
ROOT_PATH=$(dirname "${SCRIPT_PATH}")

mkdir -p "${ROOT_PATH}"/__build/llvm/wasm/debug

cd "${ROOT_PATH}"/__build/llvm/ninja/debug || exit

emcmake cmake \
  "${ROOT_PATH}"/3rdParty/llvm-project/llvm \
  -GNinja \
  -DLLVM_TARGETS_TO_BUILD= \
  -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="WebAssembly" \
  -DLLVM_USE_HOST_TOOLS:BOOL=ON \
  -DLLVM_NATIVE_PATH="${ROOT_PATH}/3rdParty/llvm/release/bin" \
  -DCMAKE_HOST_SYSTEM_NAME="Linux" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_INSTALL_PREFIX="${ROOT_PATH}"/3rdParty/llvm/wasm/debug \
  -DLLVM_INSTALL_UTILS=ON \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_ENABLE_PLUGINS=ON \
  -DLLVM_ENABLE_PROJECTS="mlir" \
  -DLLVM_ENABLE_EH=ON \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_REQUIRES_RTTI=ON \
  -DLLVM_ENABLE_PIC=ON