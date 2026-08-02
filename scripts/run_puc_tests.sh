#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SUITE="${ROOT}/tests/lua-5.3.4-tests"
if [[ -x "${ROOT}/build/luajit3" ]]; then
  LUA="${ROOT}/build/luajit3"
elif [[ -x "${ROOT}/build/Debug/luajit3" ]]; then
  LUA="${ROOT}/build/Debug/luajit3"
elif [[ -x "${ROOT}/build/Release/luajit3" ]]; then
  LUA="${ROOT}/build/Release/luajit3"
else
  echo "luajit3 binary not found; build first" >&2
  exit 1
fi
cd "$SUITE"
set +e
out="$("$LUA" -e"_U=true" all.lua 2>&1)"
code=$?
set -e
printf '%s\n' "$out"
echo "$out" | grep -q "final OK" || {
  echo "PUC basic suite failed (exit=$code): missing 'final OK'" >&2
  exit 1
}
exit 0
