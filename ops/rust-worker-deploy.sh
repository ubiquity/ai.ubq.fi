#!/usr/bin/env bash
set -euo pipefail

# The delivery controller verifies required green CI for this exact source SHA
# before invoking this authorized candidate deployment. No deployment runs in CI.
# This script validates accepted prebuilt files and never invokes a Rust tool.
fail() { printf '%s\n' "$1" >&2; exit 1; }
repo_root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
cd -- "$repo_root"
[[ -n ${CLOUDFLARE_API_TOKEN:-} ]] || fail 'The existing Cloudflare API token must be provided explicitly.'
[[ -f bun.lock && -f tools/lint/bun.lock && -f rust/Cargo.lock ]] || fail 'Frozen dependency lockfiles are required.'
[[ -z $(git status --porcelain --untracked-files=all -- rust static package.json bun.lock cloudflare.config.ts cloudflare-targets.ts wrangler.config.ts ops/rust-worker-build.sh ops/rust-worker-deploy.sh tools/cloudflare/worker-release.ts tools/cloudflare/generate-types.ts) ]] || fail 'Deployment inputs must match their committed source.'
env_directory=$repo_root
while :; do
  for env_name in .env .env.local .env.production .env.production.local .dev.vars; do
    [[ ! -e "$env_directory/$env_name" ]] || fail 'Use an isolated deployment checkout without automatically discovered dotenv files.'
  done
  [[ $env_directory != / ]] || break
  env_directory=$(dirname -- "$env_directory")
done
[[ ${NODE_OPTIONS:-} != *--env-file* ]] || fail 'Automatic Node dotenv loading is not permitted.'
bun_bin=$(command -v bun) || fail 'Prepared Bun 1.4.0 is required.'
[[ $("$bun_bin" --version) == 1.4.0 ]] || fail 'Bun 1.4.0 is required.'
worker_mode=$("$bun_bin" --no-env-file --no-install -e 'import {workerDeploymentArguments} from "./cloudflare-targets.ts"; console.log(workerDeploymentArguments(process.argv.slice(1)).target.mode);' -- "$@")
worker_name=$("$bun_bin" --no-env-file --no-install -e 'import {workerTarget} from "./cloudflare-targets.ts"; console.log(workerTarget(process.argv[1]).worker);' -- "$worker_mode")
secrets_file=$("$bun_bin" --no-env-file --no-install -e 'import {resolve} from "node:path"; import {workerDeploymentArguments} from "./cloudflare-targets.ts"; const path=workerDeploymentArguments(process.argv.slice(1)).secretsFile; console.log(path ? resolve(path) : "");' -- "$@")
secret_args=()
if [[ -n $secrets_file ]]; then
  "$bun_bin" --no-env-file --no-install -e 'import {workerTarget} from "./cloudflare-targets.ts"; import {validateDeploymentSecrets} from "./tools/cloudflare/worker-release.ts"; await validateDeploymentSecrets(process.argv[1],workerTarget(process.argv[2]));' -- "$secrets_file" "$worker_mode"
  secret_args=(--secrets-file "$secrets_file")
fi
[[ $worker_mode != rehearsal-20261106 || -n $secrets_file ]] || fail 'Rehearsal deployment requires its private admin and transfer-intent secrets file.'

"$bun_bin" --no-env-file --no-install -e 'for (const [name, version] of [["cf", "1.0.0-beta.12"], ["wrangler", "4.151.1"]]) { if ((await Bun.file(`node_modules/${name}/package.json`).json()).version !== version) throw new Error("Prepared CLI version mismatch"); }'
release_directory=$("$bun_bin" --no-env-file --no-install -e 'import {verifyWorkerRelease} from "./tools/cloudflare/worker-release.ts"; console.log(await verifyWorkerRelease(process.cwd(), process.argv[1]));' -- "$worker_mode")
[[ -z ${CLOUDFLARE_ACCOUNT_ID:-} || $CLOUDFLARE_ACCOUNT_ID == 5c0228bee348f4cf48829ffe21b61fe9 ]] || fail 'Cloudflare account mismatch.'

export CLOUDFLARE_ACCOUNT_ID=5c0228bee348f4cf48829ffe21b61fe9
export XDG_CONFIG_HOME="$repo_root/.data/rust-worker-tools/config"
export XDG_CACHE_HOME="$repo_root/.data/rust-worker-tools/cache"
export WRANGLER_LOG_PATH="$repo_root/.data/rust-worker-tools/wrangler.log"
mkdir -p -- "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME" "$(dirname -- "$WRANGLER_LOG_PATH")"
source_sha=$(git rev-parse HEAD)
cd -- "$release_directory"
exec node "$repo_root/node_modules/cf/bin/cf" deploy --prebuilt --mode "$worker_mode" --worker "$worker_name" --message "Rust gateway $source_sha" "${secret_args[@]}"
