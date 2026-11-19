#!/usr/bin/env bash
set -euo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd -P)
root=$(git -C "$here" rev-parse --show-toplevel)
revision=$(<"$here/oracle-revision.txt")
[[ $revision =~ ^[0-9a-f]{40}$ ]]

if [[ $# == 1 ]]; then
  destination=$(realpath -m -- "$1")
  if [[ $destination == "$root" || $destination == "$root/"* || -e $destination || -L $1 ]]; then
    echo 'Choose a new oracle worktree path outside the candidate checkout' >&2
    exit 2
  fi
  GIT_TERMINAL_PROMPT=0 git -C "$root" fetch --no-tags --no-write-fetch-head origin "$revision"
  git -C "$root" -c core.hooksPath=/dev/null worktree add --detach "$destination" "$revision" >&2
elif [[ $# == 2 && $1 == --check ]]; then
  destination=$(realpath -- "$2")
else
  echo 'Usage: bash oracle.sh [--check] /absolute/oracle-worktree' >&2
  exit 2
fi

if [[ $(git -C "$destination" rev-parse --show-toplevel) != "$destination" ||
      $(git -C "$destination" rev-parse HEAD) != "$revision" ||
      -n $(git -C "$destination" status --porcelain) ]] || git -C "$destination" symbolic-ref -q HEAD; then
  echo 'The oracle must be a clean detached worktree at the pinned revision' >&2
  exit 2
fi
printf '%s\n' "$revision"
