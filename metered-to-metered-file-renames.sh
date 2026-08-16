#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
cd "$ROOT"

# Process deepest paths first so files are renamed before parent directories.
find . -depth \
  -not -path './.git/*' \
  -not -path './.git' \
  \( -iname '*yunwu*' \) \
  -print0 |
while IFS= read -r -d '' path; do
  dir="$(dirname "$path")"
  base="$(basename "$path")"

  # Preserve the three casing conventions explicitly.
  newbase="${base//YUNWU/METERED}"
  newbase="${newbase//Yunwu/Metered}"
  newbase="${newbase//yunwu/metered}"

  [[ "$base" == "$newbase" ]] && continue

  newpath="$dir/$newbase"

  echo "$path -> $newpath"

  if [[ -f "$path" ]] && git ls-files --error-unmatch -- "$path" >/dev/null 2>&1; then
    git mv -- "$path" "$newpath"
  else
    mv -- "$path" "$newpath"
  fi
done

echo
echo "Remaining paths containing Yunwu:"
find . \
  -not -path './.git/*' \
  -not -path './.git' \
  -iname '*yunwu*' \
  -print

echo
echo "Git status:"
git status --short