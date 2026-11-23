#!/usr/bin/env bash
# The only permitted way to run the compiler or linker for cpp/gateway.
# This host serves production: two 2 GiB compilers or one exclusive 3 GiB linker,
# at idle priority. Supervisor v24 requires 3.5 GiB for links and 3 GiB for compiles.
# Usage: cxx.sh <g++ arguments>
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mkdir -p "$UOS_OUT/locks"
compiler="$UOS_COMPILER"
compiler_options=(--sysroot="$UOS_SYSROOT" "-B$UOS_SDK/host-tools/bin/")
mode=link
case "${1-}" in
  --vendor-c) compiler="$UOS_C_COMPILER"; shift ;;
  --vendor-archive) compiler="$UOS_SDK/host-tools/bin/ar"; compiler_options=(); mode=archive; shift ;;
esac
[ -x "$compiler" ] || { echo "cpp/gateway: SDK tool missing: $compiler" >&2; exit 2; }
for argument in "$@"; do
  case "$argument" in -c|-E|-S|-fsyntax-only) mode=compile ;; esac
done
address_limit=2147483648
if [ "$mode" = link ]; then
  # Always acquire 1 then 2: two linkers cannot each hold one and deadlock.
  exec {link_fd_one}>"$UOS_OUT/locks/compiler-1.lock"
  flock "$link_fd_one"
  exec {link_fd_two}>"$UOS_OUT/locks/compiler-2.lock"
  flock "$link_fd_two"
  address_limit=3221225472
  minimum_kb=3670016
else
  slot=""
  while [ -z "$slot" ]; do
    for candidate in 1 2; do
      exec {fd}>"$UOS_OUT/locks/compiler-$candidate.lock"
      if flock -n "$fd"; then slot=$candidate; break; fi
      exec {fd}>&-
    done
    [ -n "$slot" ] || sleep "0.$((RANDOM % 9 + 1))"
  done
  minimum_kb=3145728
fi
# Admission waits are bounded resource deferrals, never compiler diagnostics.
deadline=$((SECONDS + 1200))
while :; do
  available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
  free_kb=$(df -k --output=avail "$UOS_OUT" | tail -1)
  if [ "$free_kb" -lt 1048576 ]; then echo "cpp/gateway: under 1 GiB of disk free; refusing to $mode" >&2; exit 3; fi
  if [ "$available_kb" -lt 2097152 ]; then
    echo "cpp/gateway: live reserve exhausted: MemAvailable=${available_kb}KiB is below 2 GiB" >&2
    exit 3
  fi
  [ "$available_kb" -ge "$minimum_kb" ] && break
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo "cpp/gateway: $mode admission deferred after 1200 seconds: MemAvailable=${available_kb}KiB requires ${minimum_kb}KiB" >&2
    exit 3
  fi
  echo "cpp/gateway: $mode admission waiting: MemAvailable=${available_kb}KiB requires ${minimum_kb}KiB; retry in 30 seconds" >&2
  sleep 30
done
if [ "$mode" = link ]; then
  echo "cpp/gateway: linker admission slots=1+2 MemAvailable=${available_kb}KiB address_space=${address_limit}" >&2
fi
# shellcheck disable=SC2086
exec nice -n 19 ionice -c 3 prlimit --as="$address_limit" -- "$compiler" "${compiler_options[@]}" "$@"
