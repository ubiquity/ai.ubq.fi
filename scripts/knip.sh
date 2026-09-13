#!/bin/sh
# Report unused files, exports, types and dependencies.
#   scripts/knip.sh          -> report
#   scripts/knip.sh --fix    -> apply the fixes knip can make safely
set -eu
. "$(dirname -- "$0")/_bootstrap.sh"
ensure_deps
exec "$TOOLS/node_modules/.bin/knip" "$@"
