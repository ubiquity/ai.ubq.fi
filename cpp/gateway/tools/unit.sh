#!/usr/bin/env bash
# Build and run one test binary.
#   unit.sh <name> <source.cpp>...      paths relative to cpp/gateway
#   unit.sh --check <source.cpp>...     compile only (no link, no run)
#   unit.sh --link-only <name> <source.cpp>...   build/link only (no test execution)
# Every src/rt/*.cpp is linked automatically unless UOS_UNIT_NO_RT=1 (then list the
# runtime sources you need). Arguments after `--` go to the test.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
link_only=0
if [ "${1-}" = "--link-only" ]; then link_only=1; shift; fi
[ $# -ge 2 ] || { sed -n 2,7p "${BASH_SOURCE[0]}" >&2; exit 2; }
name="$1"; shift
sources=(); arguments=()
while [ $# -gt 0 ]; do
  if [ "$1" = "--" ]; then shift; arguments=("$@"); break; fi
  [ -f "$UOS_GATEWAY_ROOT/$1" ] || { echo "unit.sh: no such source under cpp/gateway: $1" >&2; exit 2; }
  sources+=("$1"); shift
done
common=(-s -f "$UOS_GATEWAY_ROOT/tools/unit.mk" ROOT="$UOS_GATEWAY_ROOT" OUT="$UOS_OUT" TOKEN="$$" \
  CXX_WRAPPER="$UOS_GATEWAY_ROOT/tools/cxx.sh" CXXFLAGS="$UOS_CXXFLAGS" LDLIBS="$UOS_LDLIBS")
if [ "$name" != "--check" ] && [ "${UOS_UNIT_NO_RT:-0}" != 1 ]; then
  shopt -s nullglob
  for runtime in "$UOS_GATEWAY_ROOT"/src/rt/*.cpp; do
    relative="src/rt/$(basename "$runtime")"
    case " ${sources[*]} " in *" $relative "*) ;; *) sources+=("$relative") ;; esac
  done
fi
vendor_inputs=$(python3 -B "$UOS_GATEWAY_ROOT/tools/vendor_sources.py" unit "${sources[@]}")
{
  IFS= read -r simdjson_sources
  IFS= read -r vendor_sources || true
} <<< "$vendor_inputs"
common+=(SIMDJSON_SOURCES="$simdjson_sources" VENDORS="$vendor_sources")
if [ "$name" = "--check" ]; then
  targets=()
  for source in "${sources[@]}"; do targets+=("$UOS_OUT/obj/${source%.cpp}.o"); done
  exec make "${common[@]}" SOURCES="${sources[*]}" NAME=check "${targets[@]}"
fi
make "${common[@]}" SOURCES="${sources[*]}" NAME="$name" "$UOS_OUT/bin/$name"
[ "$link_only" = 1 ] && exit 0
exec nice -n 10 timeout 300 "$UOS_LOADER" --library-path "$UOS_LOADER_PATH" "$UOS_OUT/bin/$name" "${arguments[@]}"
