#!/bin/sh
# scripts/check.sh — jaal's whole test matrix, on this machine.
#
#   scripts/check.sh            every preset below
#   scripts/check.sh dev clang  just those
#
# Presets and what they prove:
#   dev     gcc, debug                     the default
#   clang   clang, debug                   a second compiler
#   asan    address + UB sanitizers        memory errors
#   tsan    thread sanitizer               data races
#   mingw   windows cross build, run under wine
#
# Exits non-zero if any preset fails to configure, build or pass, and
# prints a one-line summary per preset at the end. Stops a hung test after
# 180 s instead of waiting forever.

set -u
cd "$(dirname "$0")/.." || exit 2

presets="${*:-dev clang asan tsan mingw}"
jobs="$(nproc 2>/dev/null || echo 4)"
summary=""
failed=0

for p in $presets; do
    printf '\n=== %s ===\n' "$p"
    start=$(date +%s)
    status="ok"
    if ! cmake --preset "$p" >"/tmp/jaal-check-$p.log" 2>&1; then
        status="configure failed"
    elif ! cmake --build --preset "$p" -j "$jobs" >>"/tmp/jaal-check-$p.log" 2>&1; then
        status="build failed"
    else
        if [ "$p" = mingw ]; then
            # wine keeps its state in ~/.wine by default; that's fine locally.
            export WINEDEBUG=-all
        fi
        if ! ctest --test-dir "build/$p" --timeout 180 -j "$jobs" \
                >>"/tmp/jaal-check-$p.log" 2>&1; then
            status="tests failed"
        fi
    fi
    took=$(( $(date +%s) - start ))
    # ctest says "100% tests passed out of 40" when all pass, and
    # "95% tests passed, 2 tests failed out of 40" when some don't.
    passed=$(grep -oE '[0-9]+% tests passed(, [0-9]+ tests failed)? out of [0-9]+' \
             "/tmp/jaal-check-$p.log" | tail -1)
    [ -n "$passed" ] || passed="-"
    if [ "$status" != ok ]; then
        failed=1
        # show why, without dumping the whole log
        grep -E 'error|\*\*\*Failed|\*\*\*Timeout|check [0-9]+ failed|FAILED' \
            "/tmp/jaal-check-$p.log" | head -15
        echo "full log: /tmp/jaal-check-$p.log"
    fi
    line=$(printf '%-6s %-18s %-45s %ss' "$p" "$status" "$passed" "$took")
    echo "$line"
    summary="$summary$line
"
done

printf '\n=== summary ===\n%s' "$summary"
exit $failed
