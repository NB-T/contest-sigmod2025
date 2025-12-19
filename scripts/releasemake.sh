#!/usr/bin/env bash

PARENT=$(cd "$(dirname "$0")/.." && pwd)

cd $PARENT
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev

cd -