#!/bin/sh
# Run ESLint with the Ubiquity ts-template ruleset.
#   scripts/lint.sh          -> report
#   scripts/lint.sh --fix    -> apply safe fixes
#
# The config lives in tools/lint/ next to the node_modules its plugins resolve
# from; see scripts/_bootstrap.sh for why the toolchain is not at the repo root.
#
# Runs the local binary rather than `npx`: npx can install ESLint, but it cannot
# make the bare plugin imports in the config resolve, so node_modules is
# required regardless and npx would only add a process spawn.
set -eu
. "$(dirname -- "$0")/_bootstrap.sh"
ensure_deps
# Regenerate the ambient Deno type declarations before linting: the type-aware
# rules silently lose findings when .deno-types.d.ts is missing or stale.
deno task types
exec "$TOOLS/node_modules/.bin/eslint" --config "$TOOLS/eslint.config.mjs" "$@"
