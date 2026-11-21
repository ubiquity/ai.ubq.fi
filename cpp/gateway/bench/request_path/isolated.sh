#!/bin/sh
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
script="$here/run.py"
if [ "${1:-}" = --codex-memory ]; then
    shift
    script="$here/../codex_memory/run.py"
fi
exec sudo -n unshare --net --pid --fork --mount-proc --kill-child=SIGKILL -- sh -eu -c '
    ip link set lo up
    uid=$1 gid=$2 script=$3 parent_netns=$4
    shift 4
    exec setpriv --reuid "$uid" --regid "$gid" --clear-groups -- \
        env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C.UTF-8 \
        python3 -I "$script" --parent-netns "$parent_netns" "$@"
' fixture "$(id -u)" "$(id -g)" "$script" "$(readlink /proc/self/ns/net)" "$@"
