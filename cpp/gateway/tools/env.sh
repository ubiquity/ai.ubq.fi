# Sourced by the cpp/gateway tool wrappers. Not an entry point.
UOS_GATEWAY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UOS_REPO_ROOT="$(cd "$UOS_GATEWAY_ROOT/../.." && pwd)"
# The qualified read-only SDK (GCC 12.2, Boost 1.81, OpenSSL 3). See cpp/README.md.
UOS_SDK="${UOS_CPP_TOOLCHAIN_DIR:-/home/codex/repos/ubiquity/ai.ubq.fi/.codex-worktrees/contract-v1-g7db25bc5c5/.data/cpp-pilot-toolchain}"
UOS_SYSROOT="$UOS_SDK/root"
UOS_TARGET_LIB="$UOS_SYSROOT/usr/lib/aarch64-linux-gnu"
UOS_COMPILER="$UOS_SYSROOT/usr/bin/aarch64-linux-gnu-g++-12"
UOS_C_COMPILER="$UOS_SYSROOT/usr/bin/aarch64-linux-gnu-gcc-12"
UOS_LOADER="$UOS_SYSROOT/lib/ld-linux-aarch64.so.1"
UOS_LOADER_PATH="$UOS_SYSROOT/lib/aarch64-linux-gnu:$UOS_TARGET_LIB"
# All build output for this worktree: shared object cache, test binaries, locks.
UOS_OUT="${UOS_GATEWAY_OUT:-$UOS_REPO_ROOT/.data/cpp-gateway}"
UOS_CXXFLAGS="-std=c++20 -O0 -g0 -Wall -Wextra -Wshadow -Wconversion -Werror -DBOOST_JSON_NO_LIB -DBOOST_CONTAINER_NO_LIB -DBOOST_SYSTEM_NO_LIB -I$UOS_GATEWAY_ROOT/include -I$UOS_GATEWAY_ROOT"
UOS_LDLIBS="$UOS_TARGET_LIB/libboost_json.a $UOS_TARGET_LIB/libboost_container.a $UOS_TARGET_LIB/libboost_system.a -lssl -lcrypto -lpthread"
for required in "$UOS_COMPILER" "$UOS_LOADER" "$UOS_SDK/host-tools/bin/ld"; do
  [ -x "$required" ] || { echo "cpp/gateway: SDK tool missing: $required" >&2; exit 2; }
done
