#!/usr/bin/env bash
# The only permitted way to run the compiler or linker for cpp/gateway.
# This host serves production: two 2 GiB compilers or one exclusive 3 GiB linker,
# at idle priority. Links need 6 GiB available after acquiring both slots.
# Usage: cxx.sh <g++ arguments>
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mkdir -p "$UOS_OUT/locks"
mode=link
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
  available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
  free_kb=$(df -k --output=avail "$UOS_OUT" | tail -1)
  if [ "$free_kb" -lt 1048576 ]; then echo "cpp/gateway: under 1 GiB of disk free; refusing to link" >&2; exit 3; fi
  if [ "$available_kb" -lt 6291456 ]; then
    echo "cpp/gateway: linker admission deferred: MemAvailable=${available_kb}KiB requires 6291456KiB after both slots" >&2
    exit 3
  fi
  address_limit=3221225472
  echo "cpp/gateway: linker admission slots=1+2 MemAvailable=${available_kb}KiB address_space=${address_limit}" >&2
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
  while :; do
    available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    free_kb=$(df -k --output=avail "$UOS_OUT" | tail -1)
    if [ "$free_kb" -lt 1048576 ]; then echo "cpp/gateway: under 1 GiB of disk free; refusing to compile" >&2; exit 3; fi
    [ "$available_kb" -ge 716800 ] && break
    sleep 2
  done
fi
# shellcheck disable=SC2086
exec nice -n 19 ionice -c 3 prlimit --as="$address_limit" -- "$UOS_COMPILER" --sysroot="$UOS_SYSROOT" "-B$UOS_SDK/host-tools/bin/" "$@"
