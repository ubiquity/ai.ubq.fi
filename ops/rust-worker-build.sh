#!/usr/bin/env bash
set -euo pipefail

fail() { printf '%s\n' "$1" >&2; exit 1; }
repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
cd -- "$repo_root"
build_host=$(uname -s):$(uname -m)
case "$build_host" in
  Linux:aarch64|Linux:arm64|Darwin:arm64) ;;
  *) fail 'Rust Worker builds require a prepared Darwin or Linux ARM64 host.' ;;
esac

env_directory=$repo_root
while :; do
  for env_name in .env .env.local .env.production .env.production.local .dev.vars; do
    [[ ! -e "$env_directory/$env_name" ]] || fail 'Use an isolated build checkout without automatically discovered dotenv files.'
  done
  [[ $env_directory != / ]] || break
  env_directory=$(dirname -- "$env_directory")
done
[[ ${NODE_OPTIONS:-} != *--env-file* ]] || fail 'Automatic Node dotenv loading is not permitted.'

for tool_directory in CARGO_HOME RUSTUP_HOME CARGO_TARGET_DIR XDG_CACHE_HOME TMPDIR; do
  tool_path=${!tool_directory:-}
  case "$tool_path" in "$repo_root"/*|/var/tmp/*|/tmp/*|/private/tmp/*) ;; *) fail 'Prepared Rust tools, targets and caches must be task-local.' ;; esac
done
for binary_name in WASM_BINDGEN_BIN WASM_OPT_BIN ESBUILD_BIN; do
  [[ -x ${!binary_name:-} ]] || fail 'All pinned Worker build-tool binary overrides must be prepared.'
done
[[ -f bun.lock && -f tools/lint/bun.lock && -f rust/Cargo.lock ]] || fail 'Frozen dependency lockfiles are required.'
[[ -z $(git status --porcelain --untracked-files=all -- rust static package.json bun.lock cloudflare.config.ts cloudflare-targets.ts wrangler.config.ts ops/rust-worker-build.sh ops/rust-worker-deploy.sh tools/cloudflare/worker-release.ts tools/cloudflare/generate-types.ts) ]] || fail 'Build inputs must match their committed source.'
[[ -z $(git status --porcelain --untracked-files=all) ]] || fail 'Exact-source Worker builds require a clean immutable Git checkout.'
build_sha=$(git rev-parse HEAD)
build_tree=$(git rev-parse 'HEAD^{tree}')
# Cargo build-script fingerprints can retain absolute Git watcher paths from a
# different immutable checkout. Never share that provenance cache across trees.
export CARGO_TARGET_DIR="$CARGO_TARGET_DIR/exact-worker/$build_sha-$build_tree"
mkdir -p -- "$CARGO_TARGET_DIR"

bun_bin=$(command -v bun) || fail 'Prepared Bun 1.4.0 is required.'
[[ $("$bun_bin" --version) == 1.4.0 ]] || fail 'Bun 1.4.0 is required.'
worker_mode=$("$bun_bin" --no-env-file --no-install -e 'import {workerTargetArguments} from "./cloudflare-targets.ts"; console.log(workerTargetArguments(process.argv.slice(1)).mode);' -- "$@")
worker_build_bin=$(command -v worker-build) || fail 'Prepared worker-build 0.8.7 is required.'
if [[ $build_host == Darwin:arm64 ]]; then
  worker_build_bin=$("$bun_bin" --no-env-file --no-install -e 'import {rustToolPath,rustVerificationEnvironment} from "./scripts/rust-verify.ts"; process.stdout.write(rustToolPath("worker-build",rustVerificationEnvironment()));')
fi
[[ $("$worker_build_bin" --version) == 0.8.7 ]] || fail 'Prepared worker-build 0.8.7 is required.'
[[ $("$WASM_BINDGEN_BIN" --version) == 'wasm-bindgen 0.2.129' ]] || fail 'Prepared wasm-bindgen 0.2.129 is required.'
"$bun_bin" --no-env-file --no-install -e 'for (const [name, version] of [["cf", "1.0.0-beta.12"], ["wrangler", "4.151.1"]]) { if ((await Bun.file(`node_modules/${name}/package.json`).json()).version !== version) throw new Error("Prepared CLI version mismatch"); }'

export XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$repo_root/.data/rust-worker-tools/config}"
export WRANGLER_LOG_PATH="$repo_root/.data/rust-worker-tools/wrangler.log"
mkdir -p -- "$XDG_CONFIG_HOME" "$(dirname -- "$WRANGLER_LOG_PATH")"
if [[ $build_host == Darwin:arm64 ]]; then
  "$bun_bin" --no-env-file --no-install -e '
import {rustToolPath,rustVerificationEnvironment} from "./scripts/rust-verify.ts";
const env=rustVerificationEnvironment();
for (const name of ["rustc","cargo"]) { const result=Bun.spawnSync([rustToolPath(name,env),"--version"],{env}); if(result.exitCode!==0 || !result.stdout.toString().startsWith(`${name} 1.98.1 `)) throw new Error(`Prepared ${name} 1.98.1 is required`); }
const child=Bun.spawnSync([rustToolPath("worker-build",env),"--release","--locked"],{cwd:"rust/crates/gateway-worker",env,stdin:"inherit",stdout:"inherit",stderr:"inherit"});
if(child.error) throw child.error;
process.exit(child.exitCode);
'
else
  (cd -- rust/crates/gateway-worker; "$worker_build_bin" --release --locked)
fi
[[ -s rust/crates/gateway-worker/build/index.js && -s rust/crates/gateway-worker/build/index_bg.wasm ]] || fail 'The prepared Rust Worker artifacts are missing.'
"$bun_bin" --no-env-file --no-install tools/cloudflare/verify-build-identity.ts "$build_sha"
"$bun_bin" --no-env-file --no-install run cf build --mode "$worker_mode"
"$bun_bin" --no-env-file --no-install run cf deploy --prebuilt --dry-run --mode "$worker_mode"
"$bun_bin" --no-env-file --no-install -e 'import {publishWorkerRelease} from "./tools/cloudflare/worker-release.ts"; console.log(await publishWorkerRelease(process.cwd(), process.argv[1]));' -- "$worker_mode"
