# C++-only gateway follow-up

The starting commit is `f92b14a97818ff81bf95cbfb2642e8ff13b4ab13` with tree `9a456b2566d6539ea817ed9c21b2b0ce7c297abf`. The deletion audit compares it with STATE A, `ca045a83ebaab6c50c20567d1a825638abac0891`.

## Deleted paths

The STATE A comparison found 57 deleted paths, 54 distinct basenames and five fully removed directories. The directory checks covered `conformance`, `packaging/confinement`, `packaging/native-kv`, `packaging/portable/licenses` and `runtime/legacy-codec` under `cpp/gateway`.

```sh
git diff --diff-filter=D --name-only --no-renames ca045a83ebaab6c50c20567d1a825638abac0891 HEAD > "$scratch/deleted-paths.txt"
git grep -I -n -F -f "$scratch/deleted-qualified.txt" --
git grep -I -n -F -f "$scratch/deleted-basenames.txt" --
git grep -a -l -F -f "$scratch/deleted-qualified.txt" -- cpp/gateway/fuzz/corpus
git grep -a -l -F -f "$scratch/deleted-basenames.txt" -- cpp/gateway/fuzz/corpus
python3 -B cpp/gateway/tools/links.py --check
```

The qualified list included each deleted path and removed directory in both root-relative and gateway-relative form. Basenames came from the deletion list. The full-tree search produced 541 qualified and 428 basename matches before distinguishing exact paths from substrings, same-name files and historical evidence. Both binary-inclusive seed searches had zero hits. The Links gate resolved all 449 registrations.

| Reference | Fix |
| --- | --- |
| `fuzz/targets.json`, `passkeys_cbor_library.extra_sources`, `src/storage/native_addon.cpp` | Removed the deleted translation unit. |
| `fuzz/targets.json`, `passkeys_asn1_library.extra_sources`, `src/storage/native_addon.cpp` | Removed the deleted translation unit. |
| `analysis/lifetime/d/tidy.py`, historical `sites.json` list including `src/kv_conversion/cli.cpp`, `src/kv_conversion/legacy_codec.cpp` and `src/kv_conversion/proof.cpp` | Discover the translation units from the current Git commit and record that commit in the lint receipt. |
| `docs/ARCHITECTURE.md`, `src/storage/native_addon.cpp` | Point to the linked SQLite backend and keep the live storage refs. |
| `docs/PORT-NOTES.md`, `tests/kv_conversion_convert_differential_test.cpp` and two references to `tests/helpers/linked_native_kv.hpp` | Document the self-contained native store fixtures. |
| `docs/briefs/file-urls.md`, `src/kv_conversion/legacy_codec.cpp`, `src/storage/native_addon.cpp`, `src/runtime/native_kv_bootstrap.cpp`, `tests/native_addon_differential_test.cpp` and `tests/kv_conversion_convert_differential_test.cpp` | Keep the native URL, static, SQLite, snapshot digest and startup checks in the example. |
| `docs/briefs/filesystem-paths.md`, `src/runtime/native_kv_bootstrap.cpp` and the retired add-on and conversion test names | Point to native startup containment and keep the registered tests in the example. |

The Markdown release ledger and the lifetime and clang-tidy JSON receipts document historical revisions. `tools/tidy.sh --audit` reads the saved clang-tidy configuration, then discovers sources directly. The first-party build, fuzz, test and package registries have no other deleted inputs. The basename `typed-value.ts` in the port manifest and port census points to the original TypeScript source under `src/kv-conversion`, which is distinct from the retired runtime shim. The conformance links resolve at STATE A.

## Release identity

`src/release.cpp` provides the build's Git SHA to `config::runtimeGitSha()`. The portable builder archives the selected commit and passes the same full SHA to the Docker build and CMake that it writes to `VERSION`. CMake sets the definition on the release translation unit. The clean macOS packaging build binds that same translation unit and reads it back in `--version`. Both packagers require the gateway Git checkout with a committed HEAD.

The portable proof compares `VERSION` with the sealed build profile and asserts the commit and platform deployment ID in the health body and headers at startup, after load and after reopen. The health handler and its deployment ID precedence use the existing code.

## Runner and commands

The four-core Ubuntu 24.04.5 ARM64 runner uses Clang 21.1.8, GCC 14.2.0, Docker 28.0.4, OpenSSL 3.0.13, zlib 1.3, Brotli 1.1.0 and Zstd 1.5.5. All temporary inputs, outputs and logs are under `/var/tmp/cppgw-cpp-only-fix-20261119.wsi2TEO0`, denoted by `$scratch` below. The LLVM key fingerprint was `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421`. The source downloads were verified against the published pins before extraction. [Dependency URLs and checksums](cppgw-input-limits-20261119.md#runner-and-exact-commands)

```sh
sudo -n apt-get -o Acquire::Retries=1 -o Acquire::https::Timeout=30 update -qq
sudo -n apt-get install -y -qq clang-21 libclang-rt-21-dev mold ninja-build pkg-config \
  libssl-dev zlib1g-dev libicu-dev libbrotli-dev libzstd-dev util-linux iproute2
sudo -n apt-get install -y -qq ccache
sudo -n apt-get install -y -qq clang-tidy-21

# In the extracted Boost source, with g++-14 selected in boost-user-config.jam.
./bootstrap.sh --with-libraries=json,container
./b2 -j2 --user-config=../boost-user-config.jam --with-json --with-container variant=release link=static threading=multi toolset=gcc-14 stage

# In the separate ICU build directory and extracted SQLite source directory, respectively.
../icu/source/configure --prefix="$scratch/icu78" --disable-tests --disable-samples --disable-extras --disable-static
make -j2
make install
./configure --prefix="$scratch/sqlite" --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'
make -j1
make install
```

The timed preparation jobs took 37.64 s for Clang and system libraries, 100.94 s for Boost 1.92.0, 147.48 s for ICU 78.3 and 64.96 s for SQLite 3.54.0. The build commands use fresh scratch directories and `env -i`; the Release driver's isolated probe reported SQLite 3.54.0.

The native code, fuzz registry and package tests run from the detached committed checkout `$scratch/source` at `e33972637da3514281e39b5a290789e77a150477`, tree `d91b53baa889409cd6f94ea004d9dab2b4c2e6d1`. The lint fix is `31688d878b59fdc3b3ee22dc9bb49a91a2f6b1df`, tree `165524a3010ff3f9b33e36d4482bed5b932164fa`. The scoped lint command passed with one translation unit and zero diagnostics in 0.75 s.

```sh
env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 HOME="$scratch/tidy-test-home" TMPDIR="$scratch/tidy-test-tmp" \
  python3 -B cpp/gateway/analysis/lifetime/d/tidy.py --boost "$scratch/boost_1_92_0" \
  --icu "$scratch/icu78/include" --source src/release.cpp --out "$scratch/tidy-scope"

bash "$scratch/release.sh" "$scratch"
bash "$scratch/fuzz.sh" "$scratch"
bash "$scratch/package.sh" "$scratch"
```

The Release selector derives the test list from `links.resolve` for `src/release.cpp`, includes startup, health and package paths, and explicitly selects both legacy passkey library tests. The CI driver selected 243 of 449 binaries and uses Linux network, mount and PID namespaces with its loopback and route-denial probes. Its logs and results are in `$scratch/ci` and `$scratch/release.log`.

The fuzz runner extracts the workflow's discovery Python from `.github/workflows/cpp-gateway-ci.yml`. The matrix contains 25 entries. It invokes `fuzz/ci.py` separately for each entry with `--seconds 20 --jobs 2` under the workflow's `sudo -n unshare --net -- setpriv` boundary. The same invocation links the target before its campaign. The `clang` and `clang++` wrappers use ccache with the installed Clang 21.1.8 compilers. Each run has a new home, temporary directory, corpus and artifact directory under `$scratch/fuzz`.

The Docker build invokes `bash cpp/gateway/packaging/portable/build.sh "$scratch/uos-gateway-linux-arm64.tar.gz"` with `PORTABLE_JOBS=3`. The same committed checkout queues `bash cpp/gateway/packaging/portable/verify.sh "$scratch/uos-gateway-linux-arm64.tar.gz" "$scratch/package-evidence"`. Both use fresh home and temp directories. Logs and exit status files begin with `package-build` and `package-verify` under `$scratch`.

## Status

Release, fuzz and Docker jobs are running. Apple Silicon execution and the automatic hosted matrix are pending. The local model review launcher requires Node and model traffic, so this job uses a local diff review. The existing public auth guide still points to the legacy upload tooling. [Previous cleanup findings](cppgw-cpp-only-20261119.md)
