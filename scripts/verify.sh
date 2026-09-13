#!/bin/sh
# Full verification gate: type generation, formatting, linting, type checking, tests.
# Runs every gate even if an earlier one fails, then exits non-zero if any failed.
set -u
. "$(dirname -- "$0")/_bootstrap.sh"
ensure_deps

fail=0

run() {
  label="$1"
  shift
  printf '\n==> %s\n' "$label"
  if ! "$@"; then
    printf '!!! FAILED: %s\n' "$label"
    fail=1
  fi
}

# Regenerate Deno's ambient types first: without .deno-types.d.ts the type-aware
# rules silently lose findings rather than erroring.
run "deno types" deno task types
run "prettier (format check)" "$TOOLS/node_modules/.bin/prettier" --check .
run "eslint (template ruleset)" "$TOOLS/node_modules/.bin/eslint" --config "$TOOLS/eslint.config.mjs" .
run "knip (unused files/exports/deps)" "$TOOLS/node_modules/.bin/knip"
run "deno fmt" deno fmt --check
run "deno lint" deno lint
run "deno check" deno task build
run "deno test" deno task test
# Mirrors CI's remaining gate, so a green verify cannot still fail the pipeline.
run "deno test (sentinel local)" deno task sentinel:test-local

printf '\n'
if [ "$fail" -ne 0 ]; then
  echo "verify: FAILED"
  exit 1
fi
echo "verify: OK"
