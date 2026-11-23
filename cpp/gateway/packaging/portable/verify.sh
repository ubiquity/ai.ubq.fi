#!/bin/bash
set -euo pipefail

if [[ $# != 2 || $(uname -sm) != 'Linux aarch64' ]]; then
  echo 'Usage on Linux ARM64 with Docker Buildx: bash verify.sh /absolute/gateway-arm64.tar.gz /absolute/new-evidence-dir' >&2
  exit 64
fi
here=$(cd -- "$(dirname -- "$0")" && pwd -P)
root=$(git -C "$here" rev-parse --show-toplevel)
archive=$(realpath -- "$1")
evidence=$(realpath -m -- "$2")
if [[ ! -f "$archive" || "$evidence" == "$root" || "$evidence" == "$root/"* || -L "$2" ]]; then
  echo 'Use an existing archive and a new evidence directory outside the checkout.' >&2
  exit 64
fi
mkdir -- "$evidence"
sha256sum -- "$archive" > "$evidence/archive.sha256"
git -C "$root" rev-parse HEAD 'HEAD^{tree}' > "$evidence/proof-source.txt"
docker version --format '{{.Server.Version}} {{.Server.Os}}/{{.Server.Arch}}' > "$evidence/docker.txt"
uname -a > "$evidence/kernel.txt"

scratch_parent=$(realpath -- "${TMPDIR:-/tmp}")
if [[ "$scratch_parent" == "$root" || "$scratch_parent" == "$root/"* ]]; then
  echo 'Use a scratch directory outside the checkout.' >&2
  exit 64
fi
scratch=$(mktemp -d --tmpdir="$scratch_parent" uos-portable-proof.XXXXXX)
container="uos-portable-${scratch##*.}"
trap 'docker rm -f "$container" >/dev/null 2>&1 || true; rm -rf -- "$scratch"' EXIT
mkdir "$scratch/context"
uid_gid="$(id -u):$(id -g)"
isolated=(--rm --platform linux/arm64 --network=none --cap-drop=ALL --security-opt=no-new-privileges:true --user "$uid_gid")

verify_release() {
  local major=$1 base=$2 ssl=$3 image cap
  local result="$evidence/debian-$major"
  local staged="$scratch/debian-$major"
  mkdir -- "$result" "$staged" || return 1
  docker buildx build --platform linux/arm64 --progress plain --load --iidfile "$staged/image-id" \
    -f "$here/Runtime.Dockerfile" --build-arg "DEBIAN=$base" --build-arg "SSL=$ssl" \
    "$scratch/context" > "$result/image-build.log" 2>&1 || return 1
  image=$(cat "$staged/image-id") || return 1
  printf '%s\n%s\n' "$base" "$image" > "$result/image.txt" || return 1
  docker run "${isolated[@]}" \
    --mount "type=bind,src=$archive,dst=/archive.tar.gz,readonly" \
    --mount "type=bind,src=$staged,dst=/staged" \
    "$image" env -i PATH=/usr/bin:/bin LC_ALL=C sh -ec '
      getconf GNU_LIBC_VERSION
      dpkg-query -W "$1" libc6 zlib1g libbrotli1 libzstd1 ca-certificates python3 binutils
      mkdir /staged/bundle
      tar --no-same-owner --no-same-permissions -xzf /archive.tar.gz -C /staged/bundle
      python3 -B /staged/bundle/seal.py /staged/package > /staged/binding.json
      cat /staged/binding.json
      for exe in /staged/package/bin/*; do
        sha256sum "$exe"
        readelf --wide --program-headers --file-header --dynamic --version-info "$exe"
        ldd -v "$exe"
        ldd -r "$exe" > /staged/relocations.txt 2>&1
        cat /staged/relocations.txt
        if grep -Eq "not found|undefined symbol" /staged/relocations.txt; then exit 1; fi
      done
    ' sh "$ssl" > "$result/elf.txt" 2>&1 || return 1
  for asset in favicon.png favicon-32.png; do
    cmp -- "$root/$asset" "$staged/package/$asset" || return 1
  done
  cp -- "$staged/package/build-profile.json" "$staged/package/runtime-closure.json" "$staged/binding.json" "$result/" || return 1

  local failed=0
  for cap in 512 256; do
    printf 'Debian %s, two CPUs, %s MiB\n' "$major" "$cap"
    if timeout --kill-after=5s 120s docker run "${isolated[@]}" --name "$container" \
        --memory="${cap}m" --memory-swap="${cap}m" --cpus=2 \
        --mount "type=bind,src=$staged,dst=/staged,readonly" \
        --mount "type=bind,src=$here/proof.py,dst=/proof.py,readonly" \
        --mount "type=bind,src=$root/cpp/gateway/tests/production_smoke,dst=/workspace/cpp/gateway/tests/production_smoke,readonly" \
        "$image" env -i PATH=/usr/bin:/bin LC_ALL=C.UTF-8 python3 -B /proof.py "$cap" \
        > "$result/${cap}MiB.json" 2> "$result/${cap}MiB.log"; then
      cat "$result/${cap}MiB.json"
    else
      failed=1
      docker rm -f "$container" >/dev/null 2>&1 || true
      cat "$result/${cap}MiB.log" >&2
    fi
  done
  return "$failed"
}

status=0
verify_release 12 debian:12@sha256:268fb24c39bca92d326dd83ea3abebe4a3f7751c804ffef23bb6d857dd343ff8 libssl3 || status=1
verify_release 13 debian:13@sha256:9c6e381421d0f48d1b8133e573f33c185ee16cbf505eda9c4a8a4791b25ea8a6 libssl3t64 || status=1
printf 'Evidence: %s\n' "$evidence"
exit "$status"
