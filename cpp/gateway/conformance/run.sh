#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# != 2 || $(uname -sm) != 'Linux aarch64' ]]; then
  echo 'Usage on Linux ARM64: bash cpp/gateway/conformance/run.sh /absolute/scratch /absolute/seal-receipt.json' >&2
  exit 2
fi
here=$(cd -- "$(dirname -- "$0")" && pwd -P)
root=$(git -C "$here" rev-parse --show-toplevel)
work=$(realpath -m -- "$1")
seal=$(realpath -- "$2")
case "$work/" in "$root/"*) echo 'Scratch must be outside the checkout' >&2; exit 2 ;; esac
if [[ -n $(git -C "$root" status --porcelain) ]]; then
  echo 'Start from a clean checkout to bind the receipt to its commit tree' >&2
  exit 2
fi
mkdir -p "$work"
launch=$(mktemp -d "$work/run.XXXXXXXX")
mkdir "$launch"/{prep-home,prep-tmp}

commands=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
prep=(env -i PATH="$commands" HOME="$launch/prep-home" TMPDIR="$launch/prep-tmp" LC_ALL=C.UTF-8)
revision=$(git -C "$root" rev-parse HEAD)
tree=$(git -C "$root" rev-parse 'HEAD^{tree}')
bench="$root/cpp/gateway/bench/request_path"
package=$(jq -er .packageRoot "$seal")
profile="$package/build-profile.json"
provenance=$(jq -er .nativeKvProvenanceSha256 "$seal")
manifest=$(jq -er .packageManifestSha256 "$seal")

digest() {
  sha256sum -- "$1" | cut -d ' ' -f 1
}
if [[ $package != "$(realpath -- "$package")" ||
      $manifest != "$(digest "$package/manifest.sha256")" ||
      $provenance != "$(digest "$package/native/kv/native-kv.provenance.json")" ||
      $provenance != "$(jq -er .nativeKvProvenanceSha256 "$package/package.json")" ||
      $(digest "$profile") != "$(jq -er .profileSha256 "$package/package.json")" ||
      $(jq -er .gitCommit "$profile") != "$revision" ||
      $(jq -er .gitTree "$profile") != "$tree" ]]; then
  echo 'The seal must bind a portable package built from this candidate commit and tree' >&2
  exit 2
fi

bun=$(realpath -- "${BUN:-$(command -v bun)}")
if [[ $("${prep[@]}" "$bun" --no-env-file --no-install --version) != 1.4.0 ]]; then
  echo 'Bun 1.4.0 is required' >&2
  exit 2
fi
source="$launch/oracle"
source_revision=$("${prep[@]}" bash "$here/oracle.sh" --production "$source")
source_tree=$(git -C "$source" rev-parse 'HEAD^{tree}')
if [[ ! -f $source/bun.lock ]]; then
  echo 'The pinned Bun application must have a root lockfile' >&2
  exit 2
fi
cmp -- "$source/tools/native-kv/inputs.json" "$package/native/kv/inputs.json"
(
  cd "$source"
  "${prep[@]}" "$bun" --no-env-file install --frozen-lockfile --ignore-scripts --backend=copyfile
) > "$launch/bun-preparation.log" 2>&1
if [[ $(jq -er .version "$source/node_modules/ws/package.json") != 8.18.3 ]]; then
  echo 'The pinned Bun application must install ws@8.18.3' >&2
  exit 2
fi

binding="$launch/oracle-binding.json"
jq -n --arg dir "$package/native/kv" --arg hash "$provenance" --arg inputs "$source/tools/native-kv/inputs.json" \
  '{artifactDirectory: $dir, expectedProvenanceSha256: $hash, inputsManifest: $inputs}' > "$binding"
cp "$bench/isolated.sh" "$launch/isolated.sh"
ln -s "$here/run.py" "$launch/run.py"
result=0
"${prep[@]}" timeout -k 10s 180s bash "$launch/isolated.sh" --bench "$bench" --benchmark-revision "$revision" \
  --source "$source" --source-revision "$source_revision" --source-tree "$source_tree" \
  --suite-revision "$revision" --suite-tree "$tree" --bun "$bun" --binding "$binding" \
  --package "$package" --out "$launch/output" || result=$?
if [[ $(git -C "$root" rev-parse HEAD) != "$revision" || -n $(git -C "$root" status --porcelain) ||
      $(digest "$package/manifest.sha256") != "$manifest" ]] ||
    ! "${prep[@]}" bash "$here/oracle.sh" --production --check "$source" >/dev/null; then
  echo "An input changed during the run. Receipt is stale: $launch/output" >&2
  exit 2
fi
echo "Conformance output: $launch/output"
exit "$result"
