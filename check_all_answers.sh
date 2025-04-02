#!/bin/bash

# Get terminal width for dynamic divider sizing
TERMINAL_WIDTH=$(tput cols)
DIVIDER=$(printf '=%.0s' $(seq 1 $TERMINAL_WIDTH))

# Function to print centered text within dividers
print_centered() {
    local text=$1
    local text_length=${#text}
    local padding=$(( (TERMINAL_WIDTH - text_length - 2) / 2 ))
    printf "=%-*s%s%*s=\n" $padding "" "$text" $padding ""
}

# Function to run a single test
run_test() {
    local test_name=$1
    local max_runs=$2
    local attempt=0
    
    echo ""
    echo "$DIVIDER"
    print_centered "RUNNING TEST $test_name"
    echo "$DIVIDER"
    echo ""

    while true; do
        attempt=$((attempt + 1))
        echo "=== Attempt #$attempt for $test_name $(printf '=%.0s' $(seq 1 $((TERMINAL_WIDTH - 20 - ${#test_name} - ${#attempt}))))"

        # Run simulator in background
        ./cache_simulator "$test_name" &
        proc_pid=$!
        sleep 1

        # Kill the process and its children
        kill -9 $proc_pid 2>/dev/null
        pkill -P $proc_pid 2>/dev/null

        # Check against reference runs
        for ((i=1; i<=$max_runs; i++)); do
            echo "  - Comparing with reference $i..."
            
            all_match=1
            for core in {0..3}; do
                # Handle different directory structures
                if [[ "$test_name" == "test_1" || "$test_name" == "test_2" ]]; then
                    ref_file="tests/$test_name/core_${core}_output.txt"
                else
                    ref_file="tests/$test_name/run_$i/core_${core}_output.txt"
                fi
                
                diff "core_${core}_output.txt" "$ref_file" > /dev/null
                if [ $? -ne 0 ]; then
                    all_match=0
                    echo "    ✗ core_${core} differs from reference $i"
                    break
                fi
            done

            if [ $all_match -eq 1 ]; then
                echo ""
                echo "$DIVIDER"
                print_centered "TEST $test_name PASSED! Matches reference $i"
                echo "$DIVIDER"
                echo ""
                return 0
            fi
        done

        echo "  × No match found in this attempt"
        echo ""
    done
}

# Main execution
echo ""
echo "$DIVIDER"
print_centered "STARTING TEST SEQUENCE"
echo "$DIVIDER"
echo ""

# Run test_1 (1 reference)
run_test "test_1" 1

# Run test_2 (1 reference)
run_test "test_2" 1

# Run test_3 (2 references)
run_test "test_3" 2

# Run test_4 (4 references)
run_test "test_4" 4

echo ""
echo "$DIVIDER"
print_centered "ALL TESTS COMPLETED SUCCESSFULLY"
echo "$DIVIDER"
echo ""