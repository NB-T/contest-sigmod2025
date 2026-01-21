#!/usr/bin/env bash

PARENT=$(cd "$(dirname "$0")/.." && pwd)

cd $PARENT  
cmake --build build --target internal_runner -- -j $(nproc)

cd -
