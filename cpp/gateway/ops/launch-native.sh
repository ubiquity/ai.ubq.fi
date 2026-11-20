#!/bin/sh
# UNINSTALLED DRAFT. This file neither selects nor replaces the live gateway.
set -eu
if test "$#" -ne 1 && test "$#" -ne 3; then
  echo 'Expected one absolute native runtime manifest path' >&2
  exit 64
fi
case "$1" in /*) ;; *) echo 'Native runtime manifest must be absolute' >&2; exit 64 ;; esac
if test "$#" -eq 3; then
  case "$2:$3" in --import-codex-auth-from:/*) ;; *) echo 'Expected --import-codex-auth-from and an absolute state directory' >&2; exit 64 ;; esac
fi
case "$0" in */*) uos_script_directory=${0%/*} ;; *) echo 'Invoke the native launcher by its path' >&2; exit 64 ;; esac
uos_package=$(CDPATH= cd -P -- "$uos_script_directory/.." && pwd)
if ! test -f "$uos_package/bin/uos-gateway" || test -L "$uos_package/bin/uos-gateway"; then
  echo 'Native gateway executable is unavailable or linked' >&2
  exit 78
fi
exec /usr/bin/env -u LD_PRELOAD -u LD_LIBRARY_PATH -u LD_AUDIT -u LD_ORIGIN_PATH \
  "$uos_package/bin/uos-gateway" --runtime-manifest "$@"
