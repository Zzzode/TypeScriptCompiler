#!/bin/bash

cd __build/llvm/ninja/wasm/release || exit

cmake --build . --config Release --target install -j 1

cmake --install . --config Release
