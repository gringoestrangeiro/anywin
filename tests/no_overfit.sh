#!/bin/bash
# Overfit audit (PLAN-CORE §2): runtime code must not name the example apps.
# Tests may mention them; runtime/ may not. Prints every offending line and fails.
# Usage: tests/no_overfit.sh
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
hits=$(grep -RniE 'todo|todos\.dat|SimpleTodoApp|TodoApp' "$root/runtime" 2>/dev/null || true)
if [ -n "$hits" ]; then
    printf '%s\n' "$hits"
    echo "no_overfit: FAIL ($(printf '%s\n' "$hits" | wc -l) line(s))"
    exit 1
fi
echo "no_overfit: PASS"
