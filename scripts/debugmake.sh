#!/usr/bin/env bash

PARENT=$(cd "$(dirname "$0")/.." && pwd)

cd $PARENT

cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNO_DUCK=OFF -Wno-dev

cd -
