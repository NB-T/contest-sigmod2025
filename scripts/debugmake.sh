#!/usr/bin/env bash

PARENT=$(cd "$(dirname "$0")/.." && pwd)

cd $PARENT

LOG_FLAG="-DENABLE_LOG=OFF"
for arg in "$@"; do
    if [ "$arg" = "--LOG" ]; then
        LOG_FLAG="-DENABLE_LOG=ON"
    fi
done

cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNO_DUCK=OFF -Wno-dev $LOG_FLAG

cd -
