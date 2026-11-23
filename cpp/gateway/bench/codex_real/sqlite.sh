#!/bin/sh
# Project the pinned CI library to the distribution path in a private mount namespace.
set -eu
umask 077
pinned=$(mktemp)
trap 'rm -f "$pinned"' EXIT
cp /usr/local/lib/libsqlite3.so.3.54.0 "$pinned"
patchelf --remove-rpath --set-soname libsqlite3.so.0 "$pinned"
chmod 644 "$pinned"
sudo -n unshare --mount --propagation private --fork --kill-child=SIGKILL -- sh -eu -c '
    uid=$1 gid=$2 sqlite=$3
    shift 3
    distro=$(readlink -f /usr/lib/aarch64-linux-gnu/libsqlite3.so.0)
    test -f "$distro"
    mount --bind "$sqlite" "$distro"
    mount -t tmpfs -o ro,nosuid,nodev,noexec tmpfs /usr/local/lib
    exec setpriv --reuid "$uid" --regid "$gid" --clear-groups --         env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C.UTF-8 "$@"
' fixture "$(id -u)" "$(id -g)" "$pinned" "$@"
