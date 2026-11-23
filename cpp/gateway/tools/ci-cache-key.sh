#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
root=$(git -C "$(dirname -- "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
{
  # Include the versioned toolchain and library packages, not just their APT aliases.
  dpkg-query -W -f='${binary:Package}\t${Version}\t${db:Status-Status}\n' |
    awk -F '\t' '
      $3 != "installed" { next }
      $1 ~ /^(binutils|ccache|clang|cpp-[0-9]|gcc|g[+][+]|libclang|libgcc|libllvm|libstdc[+][+]|llvm|mold|ninja-build|pkg-config|pkgconf)/ ||
      $1 ~ /^(libboost|libbrotli|libc6|libicu|libsqlite3|libssl|libzstd|linux-libc-dev|zlib1g)/ { print $1 "=" $2 }
    ' |
    sort
  git -C "$root" ls-tree HEAD -- cpp/gateway/cmake/vendors.json cpp/gateway/third_party
  if [[ -f /usr/local/include/sqlite3.h ]]; then
    sha256sum /usr/local/include/sqlite3.h
  fi
} | sha256sum | cut -d ' ' -f 1
