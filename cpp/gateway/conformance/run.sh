#!/usr/bin/env bash
set -euo pipefail
umask 077

if [[ $# != 1 || $(uname -sm) != 'Linux aarch64' ]]; then
  echo 'Usage on Linux ARM64: bash cpp/gateway/conformance/run.sh /absolute/scratch' >&2
  exit 2
fi
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
root=$(git -C "$here" rev-parse --show-toplevel)
work=$(realpath -m -- "$1")
case "$work/" in "$root/"*) echo 'Scratch must be outside the checkout' >&2; exit 2 ;; esac
if [[ -n $(git -C "$root" status --porcelain) ]]; then
  echo 'Start from a clean checkout to bind the receipt to its commit tree' >&2
  exit 2
fi
mkdir -p "$work"/{prep-home,prep-tmp}
exec 9>"$work/prepare.lock"
flock -n 9

revision=$(git -C "$root" rev-parse HEAD)
tree=$(git -C "$root" rev-parse 'HEAD^{tree}')
source_revision=$(git -C "$root" rev-parse --verify "${CPP_CONFORMANCE_SOURCE_REVISION:-HEAD}^{commit}")
git -C "$root" diff --quiet "$source_revision" "$revision" -- . \
  ':(exclude)cpp/gateway/conformance/**' ':(exclude)cpp/gateway/docs/CONFORMANCE.md' \
  ':(exclude)cpp/gateway/docs/findings/cppgw-conformance-e2e-20261118.md'
source="$work/oracle"
if [[ ! -d $source ]]; then
  mkdir "$source"
  git -C "$root" archive "$source_revision" | tar -x -C "$source"
  printf '%s\n' "$source_revision" > "$work/source-revision"
fi
if [[ $(cat "$work/source-revision") != "$source_revision" ]]; then
  echo 'Prepared application snapshot belongs to another revision. Use fresh scratch.' >&2
  exit 2
fi

commands=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
prep=(env -i PATH="$commands" HOME="$work/prep-home" TMPDIR="$work/prep-tmp" LC_ALL=C.UTF-8)
bench_path=cpp/gateway/bench/request_path
bench="$root/$bench_path"
bench_revision=$revision
if [[ ! -d $bench ]]; then
  bench_revision=6878261fd50c7df210308879583996bc88311724
  bench="$work/bench/$bench_path"
  if [[ ! -d $bench ]]; then
    archive_source=$root
    if ! git -C "$root" cat-file -e "$bench_revision^{commit}"; then
      archive_source="$work/bench-git"
      "${prep[@]}" git init --bare -q "$archive_source"
      "${prep[@]}" GIT_TERMINAL_PROMPT=0 GIT_CONFIG_NOSYSTEM=1 git -C "$archive_source" fetch --no-tags \
        https://github.com/ubiquity/ai.ubq.fi.git "$bench_revision"
    fi
    mkdir -p "$work/bench"
    git -C "$archive_source" archive "$bench_revision" "$bench_path" | tar -x -C "$work/bench"
  fi
fi

bun=$(realpath -- "${BUN:-$(command -v bun)}")
if [[ $("${prep[@]}" "$bun" --no-env-file --no-install --version) != 1.4.0 ]]; then
  echo 'Bun 1.4.0 is required' >&2
  exit 2
fi
if [[ ! -f $source/bun.lock || ! -f $source/tools/lint/bun.lock ]]; then
  echo 'The frozen root and lint lockfiles are required' >&2
  exit 2
fi
binding="$source/.data/native-kv.binding.json"
compiler=$(command -v "${CXX:-g++-14}")
boost=${CPP_CONFORMANCE_BOOST:-"$work/boost_1_92_0"}
if [[ ! -f $binding ]]; then
  echo 'Preparing frozen Bun dependencies and native artifacts'
  rust_version=$(jq -r .toolchain.version "$source/tools/native-kv/inputs.json")
  "${prep[@]}" RUSTUP_HOME="$work/rustup" "${RUSTUP:-$(command -v rustup)}" \
    toolchain install "$rust_version" --profile minimal --no-self-update
  mkdir -p "$work/prep-home/.rustup/toolchains"
  ln -sfn "$work/rustup/toolchains/$rust_version-aarch64-unknown-linux-gnu" \
    "$work/prep-home/.rustup/toolchains/stable-aarch64-unknown-linux-gnu"
  (
    cd "$source"
    "${prep[@]}" "$bun" --no-env-file install --frozen-lockfile --ignore-scripts
    "${prep[@]}" "$bun" --no-env-file --no-install scripts/build-permissions.ts
    "${prep[@]}" "$bun" --no-env-file --no-install scripts/prepare-native-runtime.ts
  ) > "$work/bun-preparation.log" 2>&1
fi
test -f "$source/node_modules/ws/package.json"
test -f "$source/.tmp/permissions-native/linux-arm64.node"

echo "Building native release in $work/build (log in $work/native-build.log)"
"${prep[@]}" LIBRARY_PATH="${CPP_CONFORMANCE_LIBRARY_PATH:-/usr/local/lib}" \
  timeout 40m python3 -I "$bench/build.py" --source "$source" --out "$work/build" --boost "$boost" \
  --cxx "$compiler" --jobs "${CPP_CONFORMANCE_JOBS:-4}" > "$work/native-build.log" 2>&1
package="$source/.data/conformance-package"
if [[ ! -d $package ]]; then
  "${prep[@]}" PYTHONPATH="$source/cpp/gateway/tools" python3 -B "$bench/package.py" --source "$source" \
    --build-dir "$work/build" --profile-manifest "$work/build/profile-owner.json" \
    --native-kv "$(jq -r .artifactDirectory "$binding")" --native-kv-inputs "$(jq -r .inputsManifest "$binding")" \
    --native-kv-provenance-sha256 "$(jq -r .expectedProvenanceSha256 "$binding")" \
    --permissions "$source/.tmp/permissions-native" --output "$package"
fi
cmp "$work/build/uos_gateway" "$package/bin/uos-gateway"

launch=$(mktemp -d "$work/run.XXXXXXXX")
cp "$bench/isolated.sh" "$launch/isolated.sh"
ln -s "$here/run.py" "$launch/run.py"
flock -u 9
set +e
timeout -k 10s 180s bash "$launch/isolated.sh" --bench "$bench" --benchmark-revision "$bench_revision" \
  --source "$source" --source-revision "$source_revision" --suite-revision "$revision" --suite-tree "$tree" \
  --bun "$bun" --binding "$binding" --package "$package" --out "$launch/output"
result=$?
set -e
if [[ $(git -C "$root" rev-parse HEAD) != "$revision" || -n $(git -C "$root" status --porcelain) ]]; then
  echo "Checkout changed during the run. Receipt is stale: $launch/output" >&2
  exit 2
fi
echo "Conformance output: $launch/output"
exit "$result"
