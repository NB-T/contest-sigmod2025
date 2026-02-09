#!/usr/bin/env bash
# Run timing analysis for a single job (3 repetitions, median selected)
# Usage: ./scripts/run_one.sh <plans.json> <job> [output_dir]

PLANS_PATH="${1:-}"
JOB="${2:-}"
OUTPUT_DIR="${3:-timing_output}"
NUM_REPS=3

if [ -z "$PLANS_PATH" ] || [ -z "$JOB" ]; then
    echo "Usage: $0 <plans.json> <job> [output_dir]"
    echo ""
    echo "Arguments:"
    echo "  plans.json   Path to the plans JSON file (required)"
    echo "  job          Job ID to run, e.g. 5b, 14b, 27a (required)"
    echo "  output_dir   Directory to store timing output (default: timing_output)"
    exit 1
fi

echo "============================================"
echo "Running timing for job: $JOB"
echo "Plans: $PLANS_PATH"
echo "Output: $OUTPUT_DIR"
echo "Repetitions: $NUM_REPS (median selected)"
echo "============================================"

mkdir -p "$OUTPUT_DIR"

# Extract "Total runtime: XX.XX ms" value from a timing file
extract_runtime() {
    local file="$1"
    grep -oP 'Total runtime: \K[0-9.]+' "$file" 2>/dev/null || echo "-1"
}

echo ""
echo ">>> Timing job: $JOB ($NUM_REPS repetitions)"

runtimes=()
rep_dirs=()
all_ok=true

for ((rep=1; rep<=NUM_REPS; rep++)); do
    rep_dir="${OUTPUT_DIR}/.rep_${JOB}_${rep}"
    rep_dirs+=("$rep_dir")
    mkdir -p "$rep_dir"

    echo "    Run $rep/$NUM_REPS..."
    if ! ./scripts/run_timing.sh "$PLANS_PATH" "$JOB" "$rep_dir" > /dev/null 2>&1; then
        echo "    !!! Run $rep failed"
        all_ok=false
        break
    fi

    timing_file="$rep_dir/${JOB}_timing.txt"
    if [ -f "$timing_file" ]; then
        rt=$(extract_runtime "$timing_file")
        runtimes+=("$rt")
        echo "    Run $rep: ${rt} ms"
    else
        echo "    !!! No timing file produced for run $rep"
        all_ok=false
        break
    fi
done

if [ "$all_ok" = true ] && [ ${#runtimes[@]} -eq $NUM_REPS ]; then
    # Sort runtimes and pick the median (index 1 of 3 sorted values)
    sorted=($(printf '%s\n' "${runtimes[@]}" | sort -g))
    median_idx=$(( NUM_REPS / 2 ))
    median_val="${sorted[$median_idx]}"

    # Find which rep produced the median value and copy its output
    for ((rep=0; rep<NUM_REPS; rep++)); do
        if [ "${runtimes[$rep]}" = "$median_val" ]; then
            cp "${rep_dirs[$rep]}/${JOB}_timing.txt" "$OUTPUT_DIR/${JOB}_timing.txt"
            echo "    Median: ${median_val} ms (run $((rep+1))) [${sorted[0]}, ${sorted[1]}, ${sorted[2]}]"
            break
        fi
    done

    echo ""
    echo "============================================"
    echo "Job $JOB succeeded: ${median_val} ms"
    echo "Result: $OUTPUT_DIR/${JOB}_timing.txt"
    echo "============================================"
else
    echo ""
    echo "============================================"
    echo "!!! Job $JOB failed"
    echo "============================================"
    # Clean up repetition directories before exiting
    for rep_dir in "${rep_dirs[@]}"; do
        rm -rf "$rep_dir"
    done
    exit 1
fi

# Clean up repetition directories
for rep_dir in "${rep_dirs[@]}"; do
    rm -rf "$rep_dir"
done
