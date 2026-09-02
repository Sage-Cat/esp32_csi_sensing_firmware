#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/cws-probe-timing.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -pedantic -I"$test_root/../main" \
  "$test_root/../main/cws_probe_timing.c" "$test_root/test_cws_probe_timing.c" \
  -o "$binary"
"$binary"
