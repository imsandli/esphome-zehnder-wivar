#!/usr/bin/env bash
# Build and run the host tests (needs g++): protocol layer and session logic against a fake remote.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/components/zehnder_wivar"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
# shellcheck disable=SC2054  # the comma belongs to -fsanitize
FLAGS=(-std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I"$SRC")

g++ "${FLAGS[@]}" "$ROOT/tests/test_wivar_proto.cpp" "$SRC/wivar_proto.cpp" -o "$OUT/test_wivar_proto"
"$OUT/test_wivar_proto"

g++ "${FLAGS[@]}" "$ROOT/tests/test_wivar_session.cpp" "$SRC/wivar_session.cpp" "$SRC/wivar_schedule.cpp" "$SRC/wivar_proto.cpp" \
  -o "$OUT/test_wivar_session"
"$OUT/test_wivar_session"
