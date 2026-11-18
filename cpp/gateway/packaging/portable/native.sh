#!/bin/bash
set -euo pipefail

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"

curl -fsSL --retry 3 --max-time 600 https://static.rust-lang.org/dist/rust-1.98.1-aarch64-unknown-linux-gnu.tar.xz -o rust.tar.xz
printf '%s\n' '0b514a8cc1cbcd939bff0f151661fe58b6ea5c7a7f645a5098c69e32e8c1e0a2  rust.tar.xz' | sha256sum --check
tar -xf rust.tar.xz
rust-1.98.1-aarch64-unknown-linux-gnu/install.sh --prefix="$work/rust" --disable-ldconfig \
  --components=rustc,cargo,rust-std-aarch64-unknown-linux-gnu

curl -fsSL --retry 3 --max-time 600 https://github.com/oven-sh/bun/releases/download/bun-v1.4.0/bun-linux-aarch64.zip -o bun.zip
printf '%s\n' '4b1a332ee861983eb93bcfe6f770fff94e3e31b2c388bdaea3c8ed35e58eed0e  bun.zip' | sha256sum --check
unzip -q bun.zip
mkdir "$work/home" "$work/tmp"
cd /source
kv_builder=$(cat <<'JS'
import { buildNativeKv } from "./scripts/build-native-kv.ts";
const work = Bun.env.UOS_PORTABLE_WORK;
try {
  const receipt = await buildNativeKv("/native/kv", {
    toolchainDirectory: `${work}/rust/bin`, workspaceDirectory: `${work}/kv-build`,
  });
  await Bun.write("/native/kv-provenance-sha256", `${receipt.provenanceSha256}\n`);
} catch (error) {
  for (const file of new Bun.Glob("*/build.log").scanSync(`${work}/kv-build`)) {
    console.error(await Bun.file(`${work}/kv-build/${file}`).text());
  }
  throw error;
}
JS
)
env -i PATH=/usr/bin:/bin HOME="$work/home" TMPDIR="$work/tmp" \
  UOS_PORTABLE_WORK="$work" "$work/bun-linux-aarch64/bun" --no-env-file --no-install -e "$kv_builder"

mkdir /native/permissions
permissions=/native/permissions/linux-arm64.node
cc -shared -fPIC -O2 -Wall -Wextra -Werror -march=armv8-a -DNAPI_VERSION=8 -I/usr/include/node \
  -o "$permissions" src/runtime/permissions-native.c
python3 -B - "$permissions" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

binary = Path(sys.argv[1])
manifest = {"path": binary.name, "architecture": "arm64",
            "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
            "sourceSha256": hashlib.sha256(Path("src/runtime/permissions-native.c").read_bytes()).hexdigest(),
            "landlockAbi": {"minimum": 8, "maximum": 10}}
binary.with_name("manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
