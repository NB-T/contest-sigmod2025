#!/usr/bin/env bash

if [ "$#" -ne 1 ]; then
	echo "Usage: $0 <workload ID>"
	exit 1
fi

valgrind --tool=callgrind ./build/internal_runner plans.json $1
