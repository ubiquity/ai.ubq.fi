#!/bin/sh
# Format TypeScript with the ts-template Prettier config (printWidth 160).
#   scripts/format.sh          -> write
#   scripts/format.sh --check  -> verify only
#
# Prettier owns *.ts and *.mjs only; `deno fmt` still owns JSON, Markdown, CSS,
# HTML and the static browser JS (see .prettierignore and deno.json fmt.exclude).
# Exactly one formatter per file.
#
# Runs the local binary rather than `npx`; see scripts/lint.sh for why.
set -eu
. "$(dirname -- "$0")/_bootstrap.sh"
ensure_deps

if [ "${1:-}" = "--check" ]; then
  exec "$TOOLS/node_modules/.bin/prettier" --check .
fi
exec "$TOOLS/node_modules/.bin/prettier" --write .
