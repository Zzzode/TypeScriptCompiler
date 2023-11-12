#!/bin/bash

set -ex

SCRIPT=$(readlink -f "$0")
ROOT_PATH=$(dirname "${SCRIPT}")

download_llvm() {
  echo "Downloading LLVM"
  git submodule update --init --recursive
}

configure_llvm_debug() {
  echo "Configuring LLVM (Debug)"
  "${ROOT_PATH}"/scripts/config_llvm_debug.sh
}

build_llvm_debug() {
  echo "Building LLVM (Debug)"
  "${ROOT_PATH}"/scripts/build_llvm_debug.sh
}

build_gc_debug() {
  echo "Building GC (Debug)"

  if [[ ! -d "${ROOT_PATH}"/3rdParty/gc-8.2.4 ]]; then
    curl -o gc-8.2.4.tar.gz https://www.hboehm.info/gc/gc_source/gc-8.2.4.tar.gz
    tar -xvzf gc-8.2.4.tar.gz -C "${ROOT_PATH}"/3rdParty/
  fi

  if [[ ! -d "${ROOT_PATH}"/3rdParty/libatomic_ops-7.8.0 ]]; then
    curl -o libatomic_ops-7.8.0.tar.gz https://www.hboehm.info/gc/gc_source/libatomic_ops-7.8.0.tar.gz
    tar -xvzf libatomic_ops-7.8.0.tar.gz -C "${ROOT_PATH}"/3rdParty/
    cp -a "${ROOT_PATH}"/3rdParty/libatomic_ops-7.8.0/ "${ROOT_PATH}"/3rdParty/gc-8.2.4/libatomic_ops/
  fi

  cp -a "${ROOT_PATH}"/docs/fix/gc/* "${ROOT_PATH}"/3rdParty/gc-8.2.4/
  "${ROOT_PATH}"/scripts/build_gc_debug.sh
}

main() {
  download_llvm
  configure_llvm_debug
  build_llvm_debug
  # build_gc_debug
}

# 调用主函数
main
