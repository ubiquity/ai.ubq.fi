#!/bin/bash
set -euo pipefail

if [[ $# != 1 || $(uname -sm) != 'Linux aarch64' ]]; then
  echo 'Usage on Linux ARM64 with Docker Buildx: bash build.sh /absolute/outside-checkout/gateway-arm64.tar.gz' >&2
  exit 64
fi
root=$(git -C "$(dirname -- "$0")" rev-parse --show-toplevel)
output=$(realpath -m -- "$1")
scratch_parent=$(realpath -- "${TMPDIR:-/tmp}")
if [[ "$output" == "$root" || "$output" == "$root/"* || "$scratch_parent" == "$root" || "$scratch_parent" == "$root/"* || \
      -e "$output" || -L "$1" || ! -d $(dirname -- "$output") ]]; then
  echo 'Use a new archive path and scratch directory outside the source checkout.' >&2
  exit 64
fi
jobs=${PORTABLE_JOBS:-4}
if [[ ! "$jobs" =~ ^[1-4]$ ]]; then
  echo 'PORTABLE_JOBS must be between 1 and 4.' >&2
  exit 64
fi

scratch=$(mktemp -d --tmpdir="$scratch_parent" uos-portable.XXXXXX)
archive=$(mktemp --tmpdir="$(dirname -- "$output")" .uos-portable.XXXXXX)
trap 'rm -rf -- "$scratch"; rm -f -- "$archive"' EXIT
commit=$(git -C "$root" rev-parse HEAD)
tree=$(git -C "$root" rev-parse "$commit^{tree}")
mkdir "$scratch/context"
# The context contains only committed build inputs. It never includes .env, state or a host home directory.
git -C "$root" archive "$commit" cpp/gateway scripts/build-native-kv.ts tools/native-kv \
  src/runtime/permissions-native.c src/kv-conversion static | tar -x -C "$scratch/context"

docker buildx build --platform linux/arm64 --progress plain --target package \
  -f "$scratch/context/cpp/gateway/packaging/portable/Dockerfile" \
  --build-arg "JOBS=$jobs" --build-arg "SOURCE_COMMIT=$commit" --build-arg "SOURCE_TREE=$tree" \
  --output "type=local,dest=$scratch/export" "$scratch/context"
tar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -C "$scratch/export" -cf - . | gzip -n > "$archive"
ln -T -- "$archive" "$output"
sha256sum -- "$output"
