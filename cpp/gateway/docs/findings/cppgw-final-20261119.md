# Native gateway final qualification

## Starting point

- Integration base `13502438fb2f1a986e61af3456d84611a12eed9e`, tree
  `cb6e0ccf3859245492c5bdff8107157a0dc93350`, confirmed with
  `git log -1 --format='%H %T'`. The runner checked it out as `agent-job`.
- `git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` succeeded.
  `git log -6 --oneline FETCH_HEAD` showed the residual branch headed by `a5b0e14a`.
- `uname -sm`, `cat /etc/os-release` and `nproc` showed Linux aarch64,
  Ubuntu 24.04.5 LTS and four CPUs. `sudo -n unshare --net -- true` passed.
- The checkout had no Git author configured. New commits use the existing
  branch's `Codex <codex@openai.com>` identity in local Git configuration.
- Scratch files and dependency builds go outside the checkout. Tests use the
  repository's loopback isolation and credential-free environments.

## Progress

Merged the fetched residual branch with `git merge --no-ff --no-commit FETCH_HEAD`
and ordinary two-parent merge commit `984f15ee`. It brings in `b58afd3d1` (request bounds),
`f90d511bf` (SSE cancellation), and `d35a8a8b` (allowlists). Applied `git revert --no-commit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1`.
The only conflict was in the earlier findings file, which retains its historical
results with a note pointing here. The code retains the model length checks in
both inference entry points. The two conformance cases again have the same HTTP
400 and zero-dispatch assertions on both sides. The allowlist tests were removed
by the revert. The two retained fixes and all other test registrations are present.

The runtime documentation now describes the portable and Arch build profiles,
effective bounds, the explicit systemd environment, and the build and service
environment variables. The cutover checklist uses the existing shadow unit and
deployment lock. It labels every live-data and Arch host step as unverified.
The storage runbooks now reflect the sparse and cyclic graph codec already in
the branch. Historic synthetic storage receipts remain scoped to their inputs.

The gateway guide, release, architecture, security and platform pages now
point to the current test inventory and identify prior platform results as
historical. The paired instructions distinguish the production and legacy
pins. The root `README.md` was inspected. Its paired-suite paragraph still names
the historical `2b23fb6837e2b5c0dc9ffa50771f52d322dd413a` revision as the
paired reference. The suite now reads the production pin in
`cpp/gateway/conformance/oracle-revision.txt`. The root file is outside the
authorized edit paths.

## Runner and qualification setup

The scratch root is `/tmp/cppgw-final-20261119`. The Linux ARM64 host kernel
is `7.0.0-1017-azure` from `uname -r`. Querying
`python3 -c 'import ctypes; print(ctypes.CDLL(None).syscall(444, 0, 0, 1))'`
returned Landlock ABI 8. `docker buildx version` showed v0.39.0 and
`docker version --format '{{.Server.Version}}'` showed 28.0.4.

The signed Ubuntu Noble LLVM 21 repository was checked against key fingerprint
`6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The installed tools were
`clang-21 llvm-21 libclang-rt-21-dev libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev mold`.
Clang reported 21.1.8 with `clang++-21 --version`, and the Boost build log showed the system GCC 13.3.0 toolset. The ICU build
used GCC 14.2.0. Downloaded Boost, ICU and SQLite archive SHA-256 values passed
`sha256sum --check` against the three pins in
`cpp/gateway/packaging/portable/dependencies.sh` before extraction.

```sh
# From each extracted source directory in scratch, respectively.
./bootstrap.sh --with-libraries=json,container --with-toolset=gcc
./b2 --ignore-site-config --user-config=/dev/null -j2 toolset=gcc --with-json --with-container \
  variant=release link=static runtime-link=shared cxxstd=20 cxxflags=-march=armv8-a stage
sudo install -m644 stage/lib/libboost_{json,container}.a /usr/local/lib/
sudo cp -a boost /usr/local/include/

./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1 && sudo make install && sudo ldconfig

CC=gcc-14 CXX=g++-14 ./configure --prefix=/usr/local --disable-tests --disable-samples --disable-extras
make -j2 && sudo make install && sudo ldconfig

pkg-config --modversion openssl zlib icu-uc libbrotlidec libzstd sqlite3
```

The last command reported 3.0.13, 1.3, 78.3, 1.1.0, 1.5.5 and 3.54.0.
The source archives and build output are outside the repository. The portable
Bookworm toolchain stage was warmed with `docker buildx build --platform linux/arm64 --progress plain --target toolchain --build-arg JOBS=2`
using the archived portable packaging directory. Bun 1.4.0 installed the
pinned formatter using `bun --no-env-file install --frozen-lockfile --ignore-scripts`
in a scratch copy of `package.json` and `bun.lock`.

The test driver resolved 453 tests after the revert with
`bash cpp/gateway/tools/ci-build.sh --list | wc -l`. The initial release and
sanitizer driver runs on the revert commit warmed their object caches. They were
interrupted during compilation and produced no passing test receipt.

## Qualification checkpoint

The resumed qualification started from commit
`2507e1229334f21341eb6444cfdb276eceda6f9e`, tree
`ca65880000600d53c0e1f9d61c0cb5dae0c014e6`. The package, fuzz and paired
lanes use its clean detached checkout. The suite driver uses the same source
paths as the warmup to reuse objects and records its own commit, tree and before
and after input hashes for every shard. The resumed receipts are in
`/tmp/cppgw-final-20261119/final-run.7Q5kEdU9`. Each shard gets a private
outer home and temp directory, and the driver independently uses the supported
Linux namespaces with an empty fixture home. The resumed command uses three
jobs per driver and four shards per configuration.

```bash
scratch=/tmp/cppgw-final-20261119
session=$(mktemp -d "$scratch/suite-env.XXXXXXXX")
mkdir "$session/home" "$session/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS=3 \
  UOS_CI_BUILD_DIR="$scratch/suite" UOS_CI_SQLITE_PREFIX=/usr/local \
  bash cpp/gateway/tools/ci-build.sh --config "$config" --shard "$shard/4"
```

- `python3 cpp/gateway/tools/links.py --check` exited 0 in the credential-free
  environment. `python3 -B cpp/gateway/conformance/compare_test.py` passed
  9 comparator controls.
- Stock `actionlint -color=false` 1.7.12 exited 1 on the three uses of
  `ubuntu-26.04-arm` in the existing CI workflow. GitHub's hosted runner
  inventory at `fbb26cb01c3651ea3a0347e54bfa2ef83d740d49` lists that label.
  `actionlint -config-file /tmp/cppgw-final-20261119/qualification/actionlint-platform.yml -color=false`
  exited 0 with only that label declared in the scratch config.
  [GitHub runner inventory](https://github.com/actions/runner-images/blob/fbb26cb01c3651ea3a0347e54bfa2ef83d740d49/README.md)
- The historical side-by-side harness preflight failed before launching because
  its required `/tmp/claude-1002/codex/resources.log` is absent.
  `unshare --user --map-root-user --net --mount --pid --fork --mount-proc --kill-child=SIGKILL -- true`
  independently returned `Operation not permitted` at `uid_map`. The CI driver
  supports passwordless sudo and passes its boundary check. The historical
  smoke fixture uses unprivileged user namespaces.

```bash
out=$(mktemp -d /tmp/claude-1002/prod/smoke_perf/resume.XXXXXXXX)
mkdir "$out/home" "$out/tmp"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  python3 -I cpp/gateway/tools/smoke.py --preflight --output "$out/preflight" --timeout 10
```

The first fuzz attempts for `regex`, `kv_jsc` and `json` could not link ICU:
the headers and compiled symbols were from 78.3 while the default library search
picked the distribution copy. The retry uses the workflow command with the
runner's Clang 21 selected explicitly and the prepared `/usr/local/lib` for
link and runtime lookup.

```bash
sudo -n unshare --net -- setpriv --reuid="$(id -u)" --regid="$(id -g)" --clear-groups --no-new-privs \
  env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$out/home" TMPDIR="$out/tmp" \
  CC=clang-21 CXX=clang++-21 LDFLAGS='-L/usr/local/lib -Wl,-rpath,/usr/local/lib' \
  python3 -B cpp/gateway/fuzz/ci.py "$target" --seconds 300 --jobs 4 --out "$out"
```

The candidate has 2,088 tracked files. Its first-party production C and C++
implementation and headers total 182,156 physical lines in 756 files, including
blank and comment lines. This counts the native runtime, entry points and the
shipping C/C++ platform shims. It excludes tests, benchmarks, fuzzers, third-party
sources, web assets and scripts.

```bash
git ls-files -z | tr -cd '\000' | wc -c
git ls-files -z cpp/gateway/src cpp/gateway/include cpp/gateway/runtime cpp/gateway/entrypoints \
  cpp/gateway/packaging/confinement cpp/gateway/packaging/macos |
  python3 -c 'import pathlib, sys; paths = [pathlib.Path(p.decode()) for p in sys.stdin.buffer.read().split(bytes(1)) if p and pathlib.Path(p.decode()).suffix in {".c", ".h", ".cpp", ".hpp"}]; print(len(paths), sum(len(p.read_bytes().splitlines()) for p in paths))'
```

The eight suite shards, five fuzz campaigns, portable package and Debian proofs,
and paired production comparison are in progress. No host cutover or live-data
validation has been performed.
