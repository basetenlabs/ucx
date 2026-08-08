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
#   ./run_tests_separately.sh [-f FILTER] [-s] [-t TIMEOUT] [-o OUTDIR] [-g GTEST]
#
#   -f FILTER   gtest filter selecting what to run (default: *)
#   -s          one process per test suite instead of per case
#   -t TIMEOUT  seconds allowed per process, 0 disables (default: 300)
#   -o OUTDIR   directory for logs (default: a temporary directory)
#   -g GTEST    path to the gtest binary (default: alongside this script)
#
# Startup dominates: the binary builds the variant list for every suite it
# contains before running anything, which costs about the same as
# --gtest_list_tests - tens of seconds - regardless of the filter. A process per
# case therefore pays that once per case. Use -s when a whole suite is safe to
# share a process: it keeps suites isolated from one another, which is usually
# where interference comes from, at a fraction of the cost.
#
# Cases are not run concurrently on purpose. These tests drive real devices, and
# fault-injection cases disable lanes on them, so two cases sharing a NIC can
# perturb each other.
#
# Exits non-zero if anything failed, aborted or timed out.

set -u

gtest_bin="$(dirname "$(readlink -f "$0")")/gtest"
filter="*"
timeout_s=300
outdir=""
per_suite=0

while getopts "f:st:o:g:h" opt; do
    case $opt in
        f) filter=$OPTARG ;;
        s) per_suite=1 ;;
        t) timeout_s=$OPTARG ;;
        o) outdir=$OPTARG ;;
        g) gtest_bin=$OPTARG ;;
        h) sed -n '8,40p' "$0"; exit 0 ;;
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
listing=$("$gtest_bin" --gtest_list_tests --gtest_filter="$filter" 2>/dev/null)

if [ "$per_suite" -eq 1 ]; then
    # A suite name ends in '.', and gtest treats "suite.*" as a filter for it.
    cases=$(echo "$listing" | awk '/^[^ ].*\.$/ { print $1 "*" }')
    unit="suite"
else
    cases=$(echo "$listing" |
            awk '/^[^ ].*\.$/ { suite = $1; next }
                 /^  / && suite != "" { print suite $1 }')
    unit="case"
fi

if [ -z "$cases" ]; then
    echo "error: nothing matched filter '$filter'" >&2
    exit 1
fi

total=$(echo "$cases" | wc -l | tr -d ' ')
echo "running $total $unit(s), one process each, logs in $outdir"

started=$SECONDS

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
echo "passed $passed, failed $failed, aborted $aborted, timed out $timedout" \
     "(of $total $unit(s)) in $((SECONDS - started))s"

if [ -n "$failed_names" ]; then
    echo
    echo "not ok:"
    printf '%s' "$failed_names" | sed 's/^/  /'
    exit 1
fi
