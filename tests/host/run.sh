#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
test_binary=$(mktemp "${TMPDIR:-/tmp}/littereink-history.XXXXXX")
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++11 -Wall -Wextra -Werror \
    -Itests/host/stubs tests/host/cleaning_history_test.cpp -o "$test_binary"
"$test_binary"
