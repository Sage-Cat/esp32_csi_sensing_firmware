#!/bin/sh
set -eu
test_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/cws-control-protocol.XXXXXX")
trap 'rm -f "$binary"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -pedantic -I"$test_root/../main" \
  "$test_root/../main/cws_control_protocol.c" "$test_root/../main/cws_legacy_commands.c" "$test_root/test_cws_control_protocol.c" \
  -o "$binary"
"$binary"
