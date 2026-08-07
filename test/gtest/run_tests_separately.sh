#!/bin/bash
#
# Copyright (c) NVIDIA CORPORATION & AFFILIATES, 2026. ALL RIGHTS RESERVED.
#
# See file LICENSE for terms.
#
# Run each gtest case in its own process, sequentially.
#
# The gtest binary normally runs every case in one process, which makes fault
# injection awkward to test: a case that aborts - say on an assertion in a
# teardown path - takes the whole run down with it, so the cases after it never
# report, and a case that leaves state behind can change the outcome of a later
# one. Running them separately gives each case a clean process, turns an abort
# into one failed case rather than a truncated run, and makes a per-case timeout
# possible.
#
# Usage:
#   ./run_tests_separately.sh [-f FILTER] [-t TIMEOUT] [-o OUTDIR] [-g GTEST]
#
#   -f FILTER   gtest filter selecting the cases to run (default: *)
#   -t TIMEOUT  seconds allowed per case, 0 disables (default: 300)
#   -o OUTDIR   directory for per-case logs (default: a temporary directory)
#   -g GTEST    path to the gtest binary (default: alongside this script)
#
# Exits non-zero if any case failed, aborted or timed out.

set -u

gtest_bin="$(dirname "$(readlink -f "$0")")/gtest"
filter="*"
timeout_s=300
outdir=""

while getopts "f:t:o:g:h" opt; do
    case $opt in
        f) filter=$OPTARG ;;
        t) timeout_s=$OPTARG ;;
        o) outdir=$OPTARG ;;
        g) gtest_bin=$OPTARG ;;
        h) sed -n '8,28p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

if [ ! -x "$gtest_bin" ]; then
    echo "error: gtest binary not found at $gtest_bin (pass -g)" >&2
    exit 1
fi

if [ -z "$outdir" ]; then
    outdir=$(mktemp -d "${TMPDIR:-/tmp}/gtest_separate.XXXXXX")
fi
mkdir -p "$outdir"

# --gtest_list_tests prints a suite line ending in '.', then its cases indented
# below it. A case line may carry a trailing '# GetParam() = ...' comment.
cases=$("$gtest_bin" --gtest_list_tests --gtest_filter="$filter" 2>/dev/null |
        awk '/^[^ ].*\.$/ { suite = $1; next }
             /^  / && suite != "" { print suite $1 }')

if [ -z "$cases" ]; then
    echo "error: no cases matched filter '$filter'" >&2
    exit 1
fi

total=$(echo "$cases" | wc -l | tr -d ' ')
echo "running $total case(s) separately, logs in $outdir"

passed=0
failed=0
aborted=0
timedout=0
failed_names=""

i=0
for name in $cases; do
    i=$((i + 1))
    log="$outdir/$(echo "$name" | tr '/.' '__').log"

    if [ "$timeout_s" -gt 0 ]; then
        timeout "$timeout_s" "$gtest_bin" --gtest_filter="$name" > "$log" 2>&1
    else
        "$gtest_bin" --gtest_filter="$name" > "$log" 2>&1
    fi
    rc=$?

    # 124 is timeout(1); >128 is a fatal signal, which for gtest is usually an
    # assertion in a path that gtest itself cannot report.
    case $rc in
        0)   verdict="ok";      passed=$((passed + 1)) ;;
        124) verdict="TIMEOUT"; timedout=$((timedout + 1)) ;;
        1)   verdict="FAILED";  failed=$((failed + 1)) ;;
        *)   if [ $rc -gt 128 ]; then
                 verdict="ABORTED(sig $((rc - 128)))"; aborted=$((aborted + 1))
             else
                 verdict="FAILED(rc $rc)"; failed=$((failed + 1))
             fi ;;
    esac

    if [ "$verdict" != "ok" ]; then
        failed_names="$failed_names$name [$verdict]"$'\n'
    fi
    printf '[%*d/%d] %-8s %s\n' ${#total} "$i" "$total" "$verdict" "$name"
done

echo
echo "passed $passed, failed $failed, aborted $aborted, timed out $timedout (of $total)"

if [ -n "$failed_names" ]; then
    echo
    echo "not ok:"
    printf '%s' "$failed_names" | sed 's/^/  /'
    exit 1
fi
