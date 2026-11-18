# C++ gateway CI on 2026-11-18

## Work contract v1

Run the portable Release driver with Clang on Ubuntu 26.04 ARM64 in four round-robin shards. Pushes to gateway-related branches and PRs targeting `claude/cpp-gateway-port` exercise the same suite. Each shard publishes the driver receipt, incremental results, and diagnostic logs with a short summary.

The work owns `.github/workflows/cpp-gateway-ci.yml`, `cpp/gateway/tools/ci-build.sh`, `cpp/gateway/cmake/ci/*.py`, and the two CI documents. First-party warning policy, exact Links closures, fixture namespaces, and read-only workflow permissions are invariants. Build data stays outside the checkout. Deployment and changes to the gateway, its test fixtures, and other workflows are outside the task scope.

The local qualification uses Ubuntu 26.04 x86_64 with Clang 21 and SQLite 3.54.0. It covers shard 1 and at least one additional shard. The host architecture can fail the application-level ARM64 fixtures documented in [the earlier run](cppgw-ci-build-x86-20261117.md). Any other failures need their first diagnostic and a driver fix or an out-of-scope finding. Completion requires terminal shard counts, a validated workflow, and a clean checkout. The result-branch push is the entry point for hosted ARM64 evidence.

## Initial findings

- Starting commit `45118332b287b7ba9534bb66ab1518f1990a9d9e` discovered 441 test binaries. Git recorded the wrapper as `100644`, causing the hosted jobs to exit 126 before the driver ran.
- The portable driver already reads the pinned vendor registry, builds the selected simdjson and simdutf static archives without the first-party warning flags, and links them into matching closures. The simdutf consumers include headers by their path under `third_party/`. Clang now treats those includes as system headers. Ninja explicitly tracks the vendor headers for their registered consumers because `-MMD` excludes system headers.
- The workflow originally searched for `results*.json`. The driver creates `receipt.json`, `results.tsv`, compiler logs, boundary probe logs, and each binary's link/run logs.

## Local commands

```sh
sudo apt-get update -qq
sudo apt-get install -y -qq clang ninja-build pkg-config mold util-linux libboost-json-dev libboost-container-dev \
  libssl-dev libsqlite3-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev
clang++ --version
mkdir -p "$RUNNER_TEMP/sqlite" && cd "$RUNNER_TEMP/sqlite"
curl --fail --silent --show-error --location https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz -o sqlite.tar.gz
echo "134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed  sqlite.tar.gz" | sha256sum --check
tar -xzf sqlite.tar.gz && cd sqlite-autoconf-3540000
./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3' > /dev/null
make -j4 > /dev/null && sudo make install > /dev/null && sudo ldconfig
pkg-config --modversion sqlite3
```

The disposable local preparation used `/tmp/cppgw-ci-workflow-20261118.DENd3y` in place of `$RUNNER_TEMP`. Compilation and shard results are pending.
