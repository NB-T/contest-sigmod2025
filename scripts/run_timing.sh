#!/bin/bash
# Script to run join timing analysis and output results to files
# Usage: ./scripts/run_timing.sh <path_to_plans> [query_name] [output_dir]

set -e

# Default values
PLANS_PATH="${1:-}"
QUERY_NAME="${2:-}"
OUTPUT_DIR="${3:-timing_output}"

# Check arguments
if [ -z "$PLANS_PATH" ]; then
    echo "Usage: $0 <path_to_plans> [query_name] [output_dir]"
    echo ""
    echo "Arguments:"
    echo "  path_to_plans  Path to the plans directory (required)"
    echo "  query_name     Name of specific query to run (optional, runs all if not specified)"
    echo "  output_dir     Directory to store timing output (default: timing_output)"
    echo ""
    echo "Environment variables:"
    echo "  JOIN_TIMING=1      Enable timing (automatically set by this script)"
    echo "  TIMING_OUTPUT_DIR  Override output directory"
    echo "  REPEAT             Number of repetitions (default: 3 for timing)"
    echo ""
    echo "Example:"
    echo "  $0 /path/to/plans"
    echo "  $0 /path/to/plans query_1"
    echo "  $0 /path/to/plans query_1 my_timing_output"
    exit 1
fi

# Create output directory
mkdir -p "$OUTPUT_DIR"

# Set environment variables for timing
export JOIN_TIMING=0
export TIMING_OUTPUT_DIR="$OUTPUT_DIR"
export REPEAT="${REPEAT:-3}"  # Default to 1 repeat for timing to avoid aggregating

echo "============================================"
echo "Join Timing Analysis"
echo "============================================"
echo "Plans path:    $PLANS_PATH"
echo "Query:         ${QUERY_NAME:-all queries}"
echo "Output dir:    $OUTPUT_DIR"
echo "Repetitions:   $REPEAT"
echo "============================================"

# Build the project if needed
if [ ! -f "./build/internal_runner" ]; then
    echo "Building project..."
    cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -Wno-dev
    cmake --build build --target internal_runner -- -j$(nproc)
fi

# Run the internal runner with timing
echo ""
echo "Running queries with timing enabled..."
echo ""

if [ -z "$QUERY_NAME" ]; then
    ./build/internal_runner "$PLANS_PATH"
else
    ./build/internal_runner "$PLANS_PATH" "$QUERY_NAME"
fi

echo ""
echo "============================================"
echo "Timing Results"
echo "============================================"

# List and display timing output files
if [ -d "$OUTPUT_DIR" ]; then
    for timing_file in "$OUTPUT_DIR"/*_timing.txt; do
        if [ -f "$timing_file" ]; then
            echo ""
            echo "--- $(basename "$timing_file") ---"
            cat "$timing_file"
        fi
    done
fi

echo ""
echo "Timing output files saved to: $OUTPUT_DIR/"
echo "============================================"
