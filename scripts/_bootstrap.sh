# Shared bootstrap for the dev-tooling scripts. Sourced, never executed directly.
#
# Why this exists: `npx` can install ESLint, but it cannot make the bare plugin
# imports in eslint.config.mjs resolve -- Node resolves those from the config
# file's location upward through node_modules. So node_modules is required no
# matter what, and the scripts invoke the local binaries directly.
#
# Why the toolchain lives in tools/lint/ instead of the repository root:
# this repo's JSR dependencies (@simplewebauthn/server) transitively require npm
# packages. A root package.json -- even one with an empty dependency list --
# flips Deno 2.9 into node_modules-based npm resolution, and then `deno task
# build` and `deno task test` (both --frozen, with no install step in CI) fail on
# a fresh checkout with:
#   error: Could not find a matching package for 'npm:@hexagon/base64@^1.1.27'
# Keeping package.json out of the root leaves Deno's dependency handling exactly
# as it was. Deno never looks inside tools/lint/node_modules either, because
# module resolution walks up from each source file rather than sideways.

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TOOLS="$ROOT/tools/lint"
cd "$ROOT" || exit 1

deps_ready() {
  [ -x "$TOOLS/node_modules/.bin/eslint" ] && [ -x "$TOOLS/node_modules/.bin/prettier" ] && [ -x "$TOOLS/node_modules/.bin/knip" ]
}

ensure_deps() {
  if deps_ready; then
    return 0
  fi

  echo "==> installing dev dependencies (first run)"
  for pm in bun npm; do
    command -v "$pm" >/dev/null 2>&1 || continue
    if [ "$pm" = "bun" ]; then
      (cd "$TOOLS" && bun install --no-summary)
    else
      (cd "$TOOLS" && npm install --no-audit --no-fund)
    fi
    if deps_ready; then
      return 0
    fi
    echo "==> $pm did not produce node_modules; trying the next package manager" >&2
  done

  echo "error: could not install dev dependencies (tried bun, npm)" >&2
  exit 1
}
