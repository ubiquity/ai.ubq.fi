# Paired conformance production pin

The starting revision is `541a897b3a5637da0d5b1442c84e80eb41eb9c37` on
`agent-jobs/cppgw-drift-port-20261119-44456061234-1`. The prior drift run reached 186 cases
against development only after restoring the historical `cpp/gateway/tests/production_smoke/`
fixture in its detached oracle. Without it the managed driver stopped at `ModuleNotFoundError:
No module named 'cases'`. [Production drift findings](cppgw-drift-port-20261119.md#paired-conformance)

The fixture now loads the smoke helpers, restored Bun launcher and preload-free Bun config from
the candidate checkout. The launcher takes the oracle app root explicitly and resolves `#http`
from `serve.ts` there. The frozen install checks `ws@8.18.3`. The production application's native
KV inputs still have to match the sealed package inputs. The existing nine comparator controls
passed, and Bash syntax, ShellCheck and Prettier passed on the changed launcher paths.

The fetched tip of `origin/development` and the independently queried remote ref both resolved to
`9353aa208a336da2c1516d604e0be16dc8af0ade`, tree `e349a209c455dad00d5c82579f8f69934c94d6ec`.
The production tree has no `cpp/gateway/tests/production_smoke/` entries, and its native KV inputs
match the candidate's packaged source inputs byte for byte.

```bash
scratch=/var/tmp/cppgw-conformance-pin-20261119.W1v0j49v
git fetch --no-tags origin refs/heads/development:refs/remotes/origin/development
git rev-parse 'origin/development^{commit}' 'origin/development^{tree}'
git ls-remote --exit-code origin refs/heads/development
git ls-tree origin/development cpp/gateway/tests/production_smoke
git show origin/development:tools/native-kv/inputs.json | cmp - cpp/gateway/packaging/native-kv/inputs.json
```

The regex benchmark also reads `cpp/gateway/bench/regex/oracle.mjs` from its detached oracle, and
that file is absent on `development`. The historical pin is retained for the existing oracle helper
invocation used by the regex benchmark, standalone smoke and historical tooling. Paired conformance
and the request-path benchmark now select the production pin with `oracle.sh --production`.
Both selectors created clean detached worktrees at the recorded SHAs and refused the other
selector's worktree with exit 2. The development ref was checked again during the build and
still matched the verified pin.

```bash
prep=(env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/mode-home" TMPDIR="$scratch/mode-tmp" LC_ALL=C.UTF-8)
oracle=cpp/gateway/conformance/oracle.sh
"${prep[@]}" bash "$oracle" "$scratch/legacy-check"
"${prep[@]}" bash "$oracle" --production "$scratch/production-check"
"${prep[@]}" bash "$oracle" --check "$scratch/legacy-check"
"${prep[@]}" bash "$oracle" --production --check "$scratch/production-check"
# Both cross-checks exited 2.
"${prep[@]}" bash "$oracle" --check "$scratch/production-check"
"${prep[@]}" bash "$oracle" --production --check "$scratch/legacy-check"
```

## Runner and dependencies

The runner was Ubuntu 24.04.5 ARM64 with four CPUs, Linux `7.0.0-1017-azure`, Landlock ABI 8,
Docker 28.0.4, Buildx 0.38.2 and Bun 1.4.0. The host library preparation used GCC 14.2.0.
The Bookworm portable package used Clang 21.1.5 with Boost 1.92.0, ICU 78.3 and SQLite 3.54.0.
Builds, installs and receipts were outside the checkout.

```bash
sudo -n env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME="$scratch/deps-home" TMPDIR="$scratch/deps-tmp" \
  LC_ALL=C.UTF-8 apt-get -o Acquire::Retries=2 update -qq
sudo -n env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME="$scratch/deps-home" TMPDIR="$scratch/deps-tmp" \
  LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev pkg-config util-linux iproute2

# In the scratch directory.
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/deps-home" TMPDIR="$scratch/deps-tmp" LC_ALL=C.UTF-8 \
  curl -fLsS --retry 2 https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2 -o boost_1_92_0.tar.bz2
printf '%s  %s\n' 5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c boost_1_92_0.tar.bz2 | sha256sum --check
tar -xjf boost_1_92_0.tar.bz2
printf '%s\n' 'using gcc : 14 : /usr/bin/g++-14 ;' > boost-user-config.jam
(
  cd boost_1_92_0
  env -i PATH=/usr/bin:/bin HOME="$scratch/boost-home" TMPDIR="$scratch/boost-tmp" LC_ALL=C \
    ./bootstrap.sh --with-libraries=json,container
  env -i PATH=/usr/bin:/bin HOME="$scratch/boost-home" TMPDIR="$scratch/boost-tmp" LC_ALL=C \
    ./b2 -j2 --user-config="$scratch/boost-user-config.jam" --with-json --with-container \
    variant=release link=static threading=multi toolset=gcc-14 stage
)
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$scratch/deps-home" TMPDIR="$scratch/deps-tmp" LC_ALL=C.UTF-8 \
  curl -fLsS --retry 2 https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz -o sqlite-autoconf-3540000.tar.gz
printf '%s  %s\n' 134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed sqlite-autoconf-3540000.tar.gz | sha256sum --check
tar -xzf sqlite-autoconf-3540000.tar.gz
(
  cd sqlite-autoconf-3540000
  env -i PATH=/usr/bin:/bin HOME="$scratch/sqlite-home" TMPDIR="$scratch/sqlite-tmp" LC_ALL=C \
    ./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
  env -i PATH=/usr/bin:/bin HOME="$scratch/sqlite-home" TMPDIR="$scratch/sqlite-tmp" LC_ALL=C make -j1
  env -i PATH=/usr/bin:/bin HOME="$scratch/sqlite-home" TMPDIR="$scratch/sqlite-tmp" LC_ALL=C make install
)
"$scratch/sqlite/bin/sqlite3" --version
```

Both archive SHA-256 checks passed. The host build produced `libboost_json.a` and
`libboost_container.a`, and the staged SQLite executable reported `3.54.0`.
The portable dependency stage verified and built its own pinned copies.

## Clean package comparison

The package and suite used candidate commit `ca28ab81a472b8a7c9d56543f226ba4923ddd68a` and tree
`4350dfbb9575e9146d69eb57bf8e7860757190c9`. I stopped the draft build at `2d714a9c` after the
oracle-selector follow-up superseded it. That draft exited 130 after 327.25 seconds.
The completed build took 1,310.35 seconds and finished all 396 Ninja steps. The archive was
83,915,597 bytes and passed the emitted SHA-256 check.

These commands ran from the repository root. Each runtime received a new home and temp directory.
The fixture uses synthetic credentials and loopback-only mocks. [Run instructions](../../conformance/README.md)

```bash
work="$scratch/production-gate"
test -z "$(git status --porcelain)"
git rev-parse HEAD 'HEAD^{tree}' > "$work/candidate.txt"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 HOME="$work/portable-home" \
  TMPDIR="$work/portable-tmp" DOCKER_CONFIG="$work/docker-config" \
  /usr/bin/time -o "$work/build.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash cpp/gateway/packaging/portable/build.sh "$work/uos-gateway-linux-arm64.tar.gz" \
  > "$work/SHA256SUMS" 2> "$work/build.log"
(cd "$work" && sha256sum -c SHA256SUMS)
tar --no-same-owner --no-same-permissions -xzf "$work/uos-gateway-linux-arm64.tar.gz" -C "$work/bundle"
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 HOME="$work/seal-home" TMPDIR="$work/seal-tmp" \
  python3 -B "$work/bundle/seal.py" "$work/package" > "$work/seal-receipt.json"
test -z "$(git status --porcelain)"
git rev-parse HEAD 'HEAD^{tree}' | cmp - "$work/candidate.txt"
gate_status=0
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 HOME="$work/gate-home" \
  TMPDIR="$work/gate-tmp" BUN=/home/runner/.bun/bin/bun \
  /usr/bin/time -o "$work/gate.time" -f 'wall_s=%e peak_process_kib=%M exit=%x' \
  bash cpp/gateway/conformance/run.sh "$work" "$work/seal-receipt.json" > "$work/gate.log" 2>&1 || gate_status=$?
printf '%s\n' "$gate_status" > "$work/gate.exit"
env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C.UTF-8 HOME="$work/receipt-home" TMPDIR="$work/receipt-tmp" \
  python3 -B "$scratch/verify-production-receipt.py" "$work" "$PWD" | tee "$work/verified-results.json"
```

The gate took 5.22 seconds end to end, with 162 cases before restart and 24 afterward.
The paired phase took 3.556 seconds. Both processes exited 0 in each phase, and Bun logged
`kv_closed` twice. IPv4 and IPv6 outside-route probes returned `ENETUNREACH`.
The detached production worktree was clean and contained no smoke directory. The local
Bun launcher served through the production application's `#http` and the installed `ws@8.18.3`.

All 186 case IDs were present in order. The receipt recorded 184 matches, zero cosmetic
differences, zero functional differences between gateways and two shared expectation failures.
These validation defects are in the two applications, outside the owned paths. Both cases
expect HTTP 400 and zero dispatch. The case and comparison assertions are the original ones.

| Case                          | Bun                          | Native                       |
| ----------------------------- | ---------------------------- | ---------------------------- |
| `chat-invalid-role`           | HTTP 200, `deepseek.success` | HTTP 200, `deepseek.success` |
| `responses-invalid-tool-type` | HTTP 200, `deepseek.success` | HTTP 200, `deepseek.success` |

The runner kept `results.json` and `table.md` under `$work/run.MSn8rJhD/output/`.
The receipt check verified the candidate and oracle trees, package and local fixture hashes,
both process exits in each phase, the namespace denial evidence and every case outcome.

| Evidence                   | SHA-256                                                            |
| -------------------------- | ------------------------------------------------------------------ |
| ARM64 archive              | `f518c0718f94bef6117da9fc08513dfb67de9438320102395100d35a94ec0ff3` |
| Sealed manifest            | `2eb07c1b5baa89f301ed06325c779d91463ff08156dc9ba98993c6909f17e0c2` |
| Native gateway             | `2c1f804ffe2bd75d18556d305264a05d4f94102dde11199fb334ff2c9f65fd22` |
| Bound native KV provenance | `b27aae214e855f7ebb3f1825b5a57d1f72fc2c6be0fca525ebc25b0b02f1f2e3` |
| Local Bun launcher         | `4d5bf35e0da524b5aad6a92516acdfcb9074001c05719a9584a56aba745c4085` |
| `results.json`             | `87a0b9b90a27c08963041148024e936cd34f8a304563b5f73db0f1542e03d884` |
| `table.md`                 | `2bea47a8a0cf89edca4f79f7da9b007578c6d65da0bc1e3028399fec1918de8e` |

## Other checks

The registration check resolved all 451 native tests. All nine comparator controls passed in
the loopback namespace. Bash syntax, ShellCheck, changed-file formatting and whitespace checks
passed. The formatter installed Prettier 3.9.6 from the frozen root Bun lockfile in scratch.

```bash
env -i PATH=/usr/local/bin:/usr/bin:/bin LC_ALL=C.UTF-8 HOME="$work/registration-home" TMPDIR="$work/registration-tmp" \
  python3 -B cpp/gateway/tools/links.py --check
sudo -n env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C.UTF-8 unshare --net --pid --fork --mount-proc --kill-child=SIGKILL -- sh -eu -c '
  ip link set lo up
  exec setpriv --reuid "$1" --regid "$2" --clear-groups -- \
    env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C.UTF-8 HOME="$3" TMPDIR="$4" python3 -B "$5"
' comparator "$(id -u)" "$(id -g)" "$scratch/comparator-home" "$scratch/comparator-tmp" "$PWD/cpp/gateway/conformance/compare_test.py"
bash -n cpp/gateway/conformance/oracle.sh
bash -n cpp/gateway/conformance/run.sh
shellcheck cpp/gateway/conformance/{oracle,run}.sh
git diff --check 541a897b3a5637da0d5b1442c84e80eb41eb9c37 HEAD
repo=$PWD
mapfile -t paths < <(git diff --name-only 541a897b3a5637da0d5b1442c84e80eb41eb9c37 -- cpp/gateway)
for index in "${!paths[@]}"; do paths[index]="$repo/${paths[index]}"; done
(
  cd "$scratch/formatter"
  env -i PATH=/home/runner/.bun/bin:/usr/local/bin:/usr/bin:/bin HOME="$scratch/format-home" \
    TMPDIR="$scratch/format-tmp" LC_ALL=C.UTF-8 bun --no-env-file install --frozen-lockfile --ignore-scripts
  env -i PATH=/home/runner/.bun/bin:/usr/local/bin:/usr/bin:/bin HOME="$scratch/final-format-home" \
    TMPDIR="$scratch/final-format-tmp" LC_ALL=C.UTF-8 bun --no-env-file --no-install run format:check -- "${paths[@]}"
)
```

The shared request-path launcher also passed one `small` request on each runtime using the
same package and production oracle.

```bash
oracle="$work/run.MSn8rJhD/oracle"
env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$work/benchmark-home" TMPDIR="$work/benchmark-tmp" LC_ALL=C.UTF-8 \
  sh cpp/gateway/bench/request_path/isolated.sh --source "$oracle" \
  --source-revision "$(cat cpp/gateway/conformance/oracle-revision.txt)" \
  --binding "$(dirname "$oracle")/oracle-binding.json" --bun /home/runner/.bun/bin/bun \
  --package "$work/package" --out "$work/request-path" \
  --scenarios small --requests 1 --warmup 0 --repetitions 1 --concurrency 1
```

The fetched review base was `541a897b3a5637da0d5b1442c84e80eb41eb9c37`. Against the tested
candidate above, I ran the following command inside the loopback namespace with a fresh home,
fresh temp and no credentials. Responses WebSocket connection retries ended at the 40-second
timeout with exit 124. The model review remains incomplete. The log is `$work/review.log`.

```bash
timeout -k 5s 40s codex review --base origin/agent-jobs/cppgw-drift-port-20261119-44456061234-1 -c model=gpt-6.2-sol
```

## Summary

The clean ARM64 gate ran 186 cases against `development` at
`9353aa208a336da2c1516d604e0be16dc8af0ade` in 3.556 seconds. It passed 184 cases with zero wire
differences. The two known shared failures, `chat-invalid-role` and `responses-invalid-tool-type`,
caused exit 1. Paired conformance uses the native checkout's smoke fixtures and explicitly selects
the production pin. The historical fixtures use their own pin. The isolated committed-diff review
timed out. The checkout is clean.
