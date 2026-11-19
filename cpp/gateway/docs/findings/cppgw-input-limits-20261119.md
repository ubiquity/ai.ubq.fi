# Native gateway input limits

Base branch `agent-jobs/cppgw-combine-1-20261119-44443689630-1`, commit
`e97ed985ef6c39377783e0263b10d15767de1432`, tree `95f338db7da4f5a6afc5a69c1d947c242350bfce`.

## Work and verification

- Read the native gateway README, CI driver guidance and porting rules. Inspected the clean checkout and started locating library call sites.
- `uname -m`, `cat /etc/os-release`, `g++-14 --version`, `pkg-config --modversion sqlite3 icu-uc`, and `bun --version` identified this host as Ubuntu 24.04.5 ARM64 with GCC 14.2, SQLite 3.45.1, ICU 74.2 and Bun 1.4.0. The qualified dependencies and isolated test runs are pending.
- Added encoded and decoded byte limits to the strict and Buffer base64 adapters. The encoded ceiling is the padded size of the existing 50 MiB image allowance. The safe decoder receives an output buffer capped at 50 MiB. The test covers both limits independently, including an extra decoded byte with the same encoded size.
- Bounded request bodies at the HTTP connection, including chunk declarations before payload reaches llhttp. Kept 16,408 bytes and 198 fields for server heads and trailers, the 4 KiB chunk metadata line and the larger image-edit body allowances. Added parser cases for the exact limits and one beyond.
- Applied the shared head and body limits in the response parser used by Fetch and WebSocket handshakes. Fixed-length bodies and chunk declarations are checked before payload, close-delimited responses stop at the byte limit, and chunk lines and trailers have separate allowances. Added split-head and exact-boundary parser cases.
- Added cumulative compressed and decoded byte budgets and an expansion ratio to the gzip, Brotli and Zstd paths. Fetch keeps a 16 MiB ceiling and stored gzip keeps room for replay envelopes at 64 MiB. Zstd limits its decoding window to 16 MiB. Client HTTP content encodings reach the application as raw request bytes under the existing behavior. Added budget, gzip and Brotli checks to the existing library fixture.
- Bound all QuickJS-ng patterns and subjects before conversion or execution. Fixed image, SSE framing and compaction patterns use the corresponding bounded application inputs. Connected the engine deadline to the header with an initial 1,500 ms allowance. Final tuning awaits measurements in both build profiles.
- Added Ada guards for URL and authority length, query bytes and parameter count. Long remote image paths fit the existing UTF-16 reference allowance. Query mutations check their operands and preserve the last valid value when the resulting query exceeds its budget. The URL fixture exercises exact boundaries.
- Moved the existing multipart boundary limit into the header and enforced total, part, header and field limits before copying part data or decoding text. Bounded the explicit simdjson allowance at the existing image-edit exception while keeping smaller caller limits. Added exact-boundary cases to the HTTP and JSON fixtures.
- Applied the shared CBOR size and depth before the decoder advances. Added 64 KiB certificate and 256 KiB decoded attestation-chain checks before X.509 parsing and PEM conversion. Preserved the existing ASN.1 depth, element and CRL size ceilings. The certificate fixture pads a known DER certificate to the boundary and checks one byte beyond.
- Set OpenSSL's pre-decode peer certificate-message ceiling on the shared TLS context. The aggregate TLS check also enforces the individual 64 KiB cap. Bounded local PEM stores and each decoded trust anchor, and applied the same DER byte cap to RSA SPKI and PKCS#8 imports. Added context and DER import checks.
- Applied the 16 MiB WebSocket message cap, kept smaller caller limits, and counted empty continuation frames toward a 4,096-frame ceiling. Shared sockets now expire after 60 seconds without a read or completed write. Added exact message and fragment boundary tests and a fake-clock idle test.
- Bounded cumulative chunk metadata and consecutive informational responses. Added an 8 MiB limit for decoded upstream SSE lines across socket reads, with the same allowance in the framing regexes for provider streams without a trustworthy media type. The distinct response-size error keeps passkey revocation checks fail-closed on decoder budget failures.
- Added a common text-input ceiling at the SIMD converters and a separate 16 MiB ICU input ceiling. UTF-8 validation reports oversized input as invalid and decoding raises the usual text error type. The non-throwing length helper keeps exact length by counting directly for oversized strings. Added exact-limit Unicode fixture cases.
- Bounded stored queue JSON and encoded KV cursors before parsing, reused the shared base64 adapter for the cursor wire format, and mapped a stored regex resource rejection to the existing malformed-value error. Added boundary checks in the durable queue recovery and cursor fixtures.
- Applied the same Ada query count at the inbound HTTP boundary. An oversized query now receives HTTP 400 before request construction instead of reaching the generic handler failure path.
- Added the library boundary inventory to `docs/SECURITY-LIMITS.md` and checked Ada input after Unicode normalization where its size can expand.
- Bounded composed Fetch request heads before Beast header comparisons and transmission. Added exact-size and field-count tests for outgoing heads and a loopback check for the inbound 413.
- Added Zstd output and window boundary cases using frames generated by the linked compressor, plus a stored gzip test that crosses the decoded ceiling in a second member.
- Kept direct coverage of QuickJS-ng’s allocation ceiling after the smaller subject check began rejecting the old oversized examples earlier. Added checks for the deadline supplied to the library and the full image subject allowance.

## Runner and exact commands

The scratch root is `/var/tmp/cppgw-input-limits-20261119.v8RZKRLB`. The four-core ARM64 runner uses Clang 21.1.8, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. The SQLite probes inside the production fixture boundary read 3.54.0. ICU 78.3 and Boost 1.92.0 came from verified source archives. The Boost build used GCC 14.2.0 for the static archives.

| Archive                                                                                    | SHA-256                                                            |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------ |
| `https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2`                     | `5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c` |
| `https://github.com/unicode-org/icu/releases/download/release-78.3/icu4c-78.3-sources.tgz` | `3a2e7a47604ba702f345878308e6fefeca612ee895cf4a5f222e7955fabfe0c0` |
| `https://www.sqlite.org/2026/sqlite-autoconf-3540000.tar.gz`                               | `134ec0802dda5795816e25d25872d20b312cb3973438c49b30bc40b7705ea9ed` |

Downloaded each archive using `curl -q -fLsS --retry 3 <url> -o <archive>`, verified with `sha256sum -c`, and extracted it with `tar -xf`. The signed Noble LLVM 21 repository used key fingerprint `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`.

```sh
sudo apt-get -o Acquire::Retries=2 -o Acquire::https::Timeout=20 update -qq
sudo apt-get install -y -qq clang-21 libclang-rt-21-dev mold libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux iproute2 pkg-config

# In $scratch/boost_1_92_0. The user config selects /usr/bin/g++-14.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config="$scratch/boost-user-config.jam" --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage

# In $scratch/icu-build.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install

# In $scratch/sqlite-autoconf-3540000.
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

`run-gate.sh` in the scratch root takes a profile, worker count and optional driver selection. It uses the following test command, with a new `session` directory containing empty `home` and `tmp` directories for each invocation. The driver further isolates each binary in real Linux network, mount and PID namespaces with loopback and a fresh tmpfs. The sanitizer flags supplement the driver's address, undefined-behavior and leak checks.

```sh
env -i PATH=/usr/local/bin:/usr/bin:/bin:/usr/lib/llvm-21/bin LANG=C.UTF-8 LC_ALL=C.UTF-8 TZ=UTC \
  HOME="$session/home" TMPDIR="$session/tmp" CC=clang-21 CXX=clang++-21 UOS_CI_JOBS="$workers" \
  UOS_CI_BUILD_DIR="$scratch/ci" UOS_CI_SQLITE_PREFIX="$scratch/sqlite" \
  PKG_CONFIG_PATH="$scratch/icu78/lib/pkgconfig:$scratch/sqlite/lib/pkgconfig" \
  UOS_CI_CFLAGS="$instrument" UOS_CI_CXXFLAGS="-isystem $scratch/boost_1_92_0 $instrument" \
  UOS_CI_LDFLAGS="-L$scratch/boost_1_92_0/stage/lib -Wl,--disable-new-dtags,-rpath,$scratch/icu78/lib:$scratch/sqlite/lib" \
  bash cpp/gateway/tools/ci-build.sh --config "$profile" "$@"
```

`instrument` is empty for Release and `-gline-tables-only -fno-sanitize-recover=all` for ASan. The early `$scratch/run-gate.sh release 2 rt/base64_test` and the corresponding `asan` run each observed 1 passing binary and 9 passing cases in 1,456.949 s and 1,948.448 s respectively. Source inputs changed during these cache-warming runs, and the driver invalidated both receipts. The first focused timing compile found a stale ASN.1 constant name in the distinguished-name reader. The source now uses `kCertificateBytes` there.

`python3 -B cpp/gateway/tools/links.py --check` resolved 451 registered binaries. Bun 1.4.0 installed Prettier into a scratch copy of `package.json` and `bun.lock` using `bun --no-env-file install --frozen-lockfile --ignore-scripts`. Selected Markdown files passed `bun --no-env-file --no-install run --cwd "$scratch/formatter" format:check -- "$PWD/cpp/gateway/docs/SECURITY-LIMITS.md" "$PWD/cpp/gateway/docs/findings/cppgw-input-limits-20261119.md"`. `git diff --check` passed.

## Outside the owned paths

- `docs/RUNTIME.md` and `docs/rt/regex.md` still describe several limits from the combined base, including the 2.5-second regex deadline. The current inventory lives in `SECURITY-LIMITS.md`.
- Some provider parsers search a growing SSE buffer from the beginning on each read. The transport and runtime regex checks now bound that buffer and its lines. Changes to the provider parsing loops were outside the assigned paths.
