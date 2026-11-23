#!/bin/bash
set -euo pipefail

jobs=$1
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
cd "$work"
apt-get update
apt-get install -y --no-install-recommends gpg bzip2 cmake ninja-build mold pkg-config \
  libssl-dev zlib1g-dev libbrotli-dev libzstd-dev

# Verify the LLVM archive key before allowing APT to use it.
export GNUPGHOME="$work/gnupg"
install -d -m700 "$GNUPGHOME"
curl -fsSL --retry 3 https://apt.llvm.org/llvm-snapshot.gpg.key -o llvm.asc
gpg --batch --with-colons --show-keys llvm.asc | awk -F: '
  $1 == "pub" { primary = 1 }
  $1 == "fpr" && primary { print $10; primary = 0 }
' | diff - <(printf '%s\n' 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421)
install -m644 llvm.asc /usr/share/keyrings/uos-llvm.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/uos-llvm.asc] https://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-21 main' \
  > /etc/apt/sources.list.d/uos-llvm.list
apt-get -o Acquire::Retries=3 update
apt-get -o Acquire::Retries=3 install -y --no-install-recommends clang-21
clang++-21 --version
rm -rf /var/lib/apt/lists/*

# Boost's bootstrap clears CXX. Put compiler wrappers on PATH so both
# bootstrap and the library builds use the same launcher selection.
launcher=$(PYTHONPATH=/opt/uos/ci-tools python3 -B -c \
  'import compiler_cache, shlex; print(shlex.join(compiler_cache.launcher()))')
if [[ -n "$launcher" ]]; then
  mkdir "$work/compiler-launchers"
  for compiler in cc gcc g++ clang-21 clang++-21; do
    compiler_path=$(command -v "$compiler")
    cat > "$work/compiler-launchers/$compiler" <<EOF
#!/bin/sh
for argument do
  case "\$argument" in
    -c|*.c|*.cc|*.cpp|*.cxx|@*) exec $launcher "$compiler_path" "\$@" ;;
  esac
done
exec "$compiler_path" "\$@"
EOF
    chmod +x "$work/compiler-launchers/$compiler"
  done
  export PATH="$work/compiler-launchers:$PATH"
fi

download() {
  curl -fsSL --retry 3 --max-time 600 "$1" -o "$2"
  printf '%s  %s\n' "$3" "$2" | sha256sum --check
}

download https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 boost.tar.bz2 \
  5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c
mkdir -p /opt/uos/boost-1.92.0
tar -xjf boost.tar.bz2 --strip-components=1 -C /opt/uos/boost-1.92.0
(
  cd /opt/uos/boost-1.92.0
  ./bootstrap.sh --with-libraries=json,container --with-toolset=gcc
  ./b2 --ignore-site-config --user-config=/dev/null -j"$jobs" toolset=gcc \
    --with-json --with-container variant=release link=static runtime-link=shared cxxstd=20 \
    cxxflags=-march=armv8-a stage
  install -m644 stage/lib/libboost_{json,container}.a /usr/lib/
)

download https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz sqlite.tar.gz \
  134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed
tar -xzf sqlite.tar.gz
(
  cd sqlite-autoconf-3540000
  CC=clang-21 CFLAGS='-O2 -march=armv8-a -DSQLITE_DQS=3' \
    ./configure --prefix=/opt/uos --disable-shared --enable-static --disable-readline
  make -j"$jobs" libsqlite3.a
  install -Dm644 libsqlite3.a /opt/uos/lib/libsqlite3.a
  install -Dm644 sqlite3.h /opt/uos/include/sqlite3.h
  install -Dm644 sqlite3ext.h /opt/uos/include/sqlite3ext.h
)

download https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz icu.tgz \
  3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0
tar -xzf icu.tgz
(
  cd icu/source
  CC=clang-21 CXX=clang++-21 CFLAGS='-O2 -march=armv8-a' CXXFLAGS='-O2 -march=armv8-a' \
    ./configure --prefix=/opt/uos --disable-shared --enable-static --with-data-packaging=static \
      --disable-tests --disable-samples --disable-extras
  make -j"$jobs"
  make install
)
install -Dm644 icu/LICENSE /opt/uos/licenses/icu/LICENSE
install -Dm644 /opt/uos/boost-1.92.0/LICENSE_1_0.txt /opt/uos/licenses/boost/LICENSE
install -Dm644 /usr/share/doc/libstdc++6/copyright /opt/uos/licenses/libstdc++/copyright
install -Dm644 /usr/share/doc/libgcc-s1/copyright /opt/uos/licenses/libgcc/copyright
install -Dm644 /usr/share/doc/libc6/copyright /opt/uos/licenses/glibc/copyright
install -Dm644 /usr/share/doc/libssl-dev/copyright /opt/uos/licenses/openssl/copyright
install -Dm644 /usr/share/doc/zlib1g-dev/copyright /opt/uos/licenses/zlib/copyright
install -Dm644 /usr/share/doc/libbrotli-dev/copyright /opt/uos/licenses/brotli/copyright
install -Dm644 /usr/share/doc/libzstd-dev/copyright /opt/uos/licenses/zstd/copyright
