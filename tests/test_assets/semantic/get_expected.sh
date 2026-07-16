#!/usr/bin/env bash

set -u

TEST_ROOT="tests/test_assets/semantic/invalid"
RUNNER="./cmake-build-debug/check_semantics"

if [[ ! -x "$RUNNER" ]]; then
    echo "error: runner not found or not executable: $RUNNER" >&2
    exit 2
fi

if [[ ! -d "$TEST_ROOT" ]]; then
    echo "error: test directory not found: $TEST_ROOT" >&2
    exit 2
fi

generated=0
failed=0

while IFS= read -r -d '' file; do
    expected="${file%.cog}.expected"
    temporary="${expected}.tmp"

    "$RUNNER" "$file" 2> "$temporary"
    status=$?

    if [[ "$status" -eq 1 ]]; then
        mv "$temporary" "$expected"
        echo "generated: $expected"
        ((generated += 1))
    else
        rm -f "$temporary"
        echo "unexpected exit code $status: $file" >&2
        ((failed += 1))
    fi
done < <(find "$TEST_ROOT" -type f -name '*.cog' -print0 | sort -z)

echo
echo "generated: $generated"
echo "failed:    $failed"

if [[ "$failed" -ne 0 ]]; then
    exit 1
fi