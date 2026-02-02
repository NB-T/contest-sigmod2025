#!/usr/bin/env bash
# Run timing analysis for a representative subset of 15 jobs (3 repetitions, median selected)
# Selected via stratified sampling across log-runtime quantiles with structural diversity.
# Covers full runtime range (1.48-292.17 ms), 12/15 unique pipeline profiles,
# 14 query groups, and tracks the full-set median within 0.3 ms.
# Usage: ./scripts/quick_run.sh <plans.json> [output_dir]

PLANS_PATH="${1:-}"
OUTPUT_DIR="${2:-timing_output}"
NUM_REPS=3

if [ -z "$PLANS_PATH" ]; then
    echo "Usage: $0 <plans.json> [output_dir]"
    echo ""
    echo "Arguments:"
    echo "  plans.json   Path to the plans JSON file (required)"
    echo "  output_dir   Directory to store timing output (default: timing_output)"
    exit 1
fi

jobs=("5b" "14b" "27a" "10b" "21a" "32b" "29b" "33c" "15d" "18c" "29c" "17c" "20c" "16b" "8c")

failed_jobs=()
succeeded_jobs=()

echo "============================================"
echo "Running timing for ${#jobs[@]} representative jobs"
echo "Plans: $PLANS_PATH"
echo "Output: $OUTPUT_DIR"
echo "Repetitions per query: $NUM_REPS (median selected)"
echo "============================================"

mkdir -p "$OUTPUT_DIR"

# Extract "Total runtime: XX.XX ms" value from a timing file
extract_runtime() {
    local file="$1"
    grep -oP 'Total runtime: \K[0-9.]+' "$file" 2>/dev/null || echo "-1"
}

for job in "${jobs[@]}"; do
    echo ""
    echo ">>> Timing job: $job ($NUM_REPS repetitions)"

    runtimes=()
    rep_dirs=()
    all_ok=true

    for ((rep=1; rep<=NUM_REPS; rep++)); do
        rep_dir="${OUTPUT_DIR}/.rep_${job}_${rep}"
        rep_dirs+=("$rep_dir")
        mkdir -p "$rep_dir"

        echo "    Run $rep/$NUM_REPS..."
        if ! ./scripts/run_timing.sh "$PLANS_PATH" "$job" "$rep_dir" > /dev/null 2>&1; then
            echo "    !!! Run $rep failed"
            all_ok=false
            break
        fi

        timing_file="$rep_dir/${job}_timing.txt"
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
                cp "${rep_dirs[$rep]}/${job}_timing.txt" "$OUTPUT_DIR/${job}_timing.txt"
                echo "    Median: ${median_val} ms (run $((rep+1))) [${sorted[0]}, ${sorted[1]}, ${sorted[2]}]"
                break
            fi
        done

        succeeded_jobs+=("$job")
    else
        echo "!!! Job $job failed"
        failed_jobs+=("$job")
    fi

    # Clean up repetition directories
    for rep_dir in "${rep_dirs[@]}"; do
        rm -rf "$rep_dir"
    done
done

echo ""
echo "============================================"
echo "Summary"
echo "============================================"
echo "Succeeded: ${#succeeded_jobs[@]} jobs"
echo "Failed:    ${#failed_jobs[@]} jobs"
if [ ${#failed_jobs[@]} -gt 0 ]; then
    echo "Failed jobs: ${failed_jobs[*]}"
fi
echo "Results in: $OUTPUT_DIR"
echo "============================================"
