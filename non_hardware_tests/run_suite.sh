#!/usr/bin/env bash

set -o pipefail

# Accept alternate build/output paths for sanitizer and local watch workflows.
test_build_dir="${1:-build/non_hardware}"
test_output="${2:-out.txt}"

ctest --test-dir "${test_build_dir}" --verbose 2>&1 | tee "${test_output}"
ctest_status=${PIPESTATUS[0]}

# Count individual harness cases instead of CTest executables.
read -r passed failed < <(
    awk '
        /^[0-9]+: \[PASS\]/ { passed++ }
        /^[0-9]+: \[FAIL\]/ { failed++ }
        END { print passed + 0, failed + 0 }
    ' "${test_output}"
)
total=$((passed + failed))

{
    echo
    echo "Non-hardware test cases: ${total} total, ${passed} passed, ${failed} failed"
} | tee -a "${test_output}"

if ((ctest_status != 0 || failed != 0)); then
    exit 1
fi
