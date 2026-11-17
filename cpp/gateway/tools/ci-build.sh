#!/usr/bin/env bash
# Portable disposable-runner build; deliberately independent of the production SDK.
set -euo pipefail
ci_tools_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 -B "$ci_tools_dir/../cmake/ci/driver.py" "$@"
