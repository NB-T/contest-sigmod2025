#!/usr/bin/env bash

PARENT=$(cd "$(dirname "$0")/.." && pwd)

cd $PARENT  
cmake --build build -- -j $(nproc)

cd -