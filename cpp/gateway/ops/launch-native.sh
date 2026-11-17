#!/bin/sh
# UNINSTALLED DRAFT. This file neither selects nor replaces the live gateway.
set -eu
if test "$#" -ne 1; then
  echo 'Expected one absolute native runtime manifest path' >&2
  exit 64
fi
case "$1" in /*) ;; *) echo 'Native runtime manifest must be absolute' >&2; exit 64 ;; esac
uos_package=$(CDPATH= cd -P -- "$(dirname -- "$0")/.." && pwd)
if ! test -f "$uos_package/bin/uos-gateway" || test -L "$uos_package/bin/uos-gateway"; then
  echo 'Native gateway executable is unavailable or linked' >&2
  exit 78
fi
exec /usr/bin/env -u LD_PRELOAD -u LD_LIBRARY_PATH -u LD_AUDIT -u LD_ORIGIN_PATH -u BUN_OPTIONS -u NODE_OPTIONS -u NODE_PATH \
  "$uos_package/bin/uos-gateway" --runtime-manifest "$1"
