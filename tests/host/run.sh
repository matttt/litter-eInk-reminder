#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_binary=$(mktemp "${TMPDIR:-/tmp}/littereink-history.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
for test_source in tests/host/*_test.cpp; do
    "${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
        -Itests/host/stubs "$test_source" -o "$test_binary"
    "$test_binary"
done
