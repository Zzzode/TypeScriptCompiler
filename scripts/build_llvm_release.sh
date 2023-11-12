#!/bin/bash

cd __build/llvm/ninja/release

cmake --build . --config Release --target install -j

cmake --install . --config Release
