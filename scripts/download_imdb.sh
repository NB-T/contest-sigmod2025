#!/usr/bin/env bash

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)

url="https://event.cwi.nl/da/job/imdb.tgz"
output_file="${ROOT_DIR}/imdb.tgz"
target_dir="${ROOT_DIR}/imdb"

echo "Downloading IMDB dataset to ${output_file}"

# Detect and select downloader
if command -v wget &> /dev/null; then
    if ! wget "$url" -O "$output_file"; then
        echo "Error: downloading failed" >&2
        exit 1
    fi
elif command -v curl &> /dev/null; then
    if ! curl -L "$url" -o "$output_file"; then
        echo "Error: downloading failed" >&2
        exit 1
    fi
else
    echo "Error: please install wget or curl to download imdb.tgz" >&2
    exit 1
fi

# make target directory (if not exists)
if ! mkdir -p "$target_dir"; then
    echo "Error: cannot make directory '$target_dir'" >&2
    exit 1
fi

# decompress the file to the target directory
if ! tar -xf "$output_file" -C "$target_dir"; then
    echo "Error: failed to decompress the file" >&2
    exit 1
fi

echo "Success!"
