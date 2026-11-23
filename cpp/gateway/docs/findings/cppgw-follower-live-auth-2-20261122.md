# Live Codex login for a same-host follower

Base is `fe308a3435a58cd2a0462a3117f54cdce80d3b97` on `claude/cpp-gateway-port`. The runner is Ubuntu 26.04 ARM64. All inference and usage fixtures use synthetic credentials and local mocks.

- [x] Share credential validation between stopped import and the live reader.
- [x] Open the live source without write access and confine it to a separate state tree.
- [x] Wire startup, demand-driven recovery, durable adoption and health.
- [x] Verify with a concurrent WAL writer, loopback inference and Linux confinement in Release.
- [x] Document operator setup and record Release and registration receipts.
- [x] Verify the sealed launcher with live state and loopback inference.
- [x] Record ASan/UBSan receipts.

## Code and registration

Tested code commit `31a78c546f85631ef48f964811d0126a82da7a62`, tree `33629a6849a77813b5765ba9749ce821cf93f0e2`. Later findings and wording commits are documentation only. Clang 21.1.8 built on Ubuntu 26.04.1 ARM64. SQLite 3.54.0 was built from the CI tarball after SHA-256 verification, with `-O2 -DSQLITE_DQS=3`. The fixture printed `sqlite3_libversion(): 3.54.0` from inside the boundary.

`python3 -B cpp/gateway/tools/links.py --check` passed for 453 native test registrations. `python3 -B cpp/gateway/tools/cmake-sources.py /var/tmp/cppgw-follower-live-registered-sources.cmake` found 368 production sources, including 45 runtime sources. `git diff --check origin/claude/cpp-gateway-port...HEAD` passed.

## Release

Each row ran `env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 CC=clang CXX=clang++ UOS_CI_JOBS=4 bash cpp/gateway/tools/ci-build.sh --config release <filter>`. Every receipt matched the code commit and tree and recorded unchanged inputs. Each test ran as an unprivileged user in private Linux network, mount and PID namespaces with a fresh home and temp directory and loopback-only network.

| Filter | Binaries passed | Cases passed |
| --- | ---: | ---: |
| `codex_auth` | 9 | 138 |
| `codex_external` | 2 | 11 |
| `codex_native_auth` | 1 | 31 |
| `codex_serial_routing_http` | 1 | 2 |
| `gemini_permissions` | 1 | 4 |
| `live_` | 4 | 57 |
| `permissions_linux` | 3 | 13 |
| `runtime_native_startup` | 1 | 1 |
| `sqlite` | 3 | 54 |
| **Total** | **25** | **311** |

The follower HTTP fixture uses a separate process with an open WAL writer. Its forced truncate checkpoints succeeded during both adoption probes and the rejected-candidate probe. Landlock denied writes to all three source files, journal creation, unlink and a read of a leader file outside `.data`. The recursive listing and metadata for other leader files matched. The follower recovered an early 401 after the five-second read interval, kept serving through busy, closed, missing and garbage source states, and reopened its own database with the last adopted login. The provider mock saw zero OAuth requests. The separate SQLite case read from a directory with no write permission and confirmed that the reader did not recreate a missing shared-memory file.

## ASan, UBSan and LSan

The same nine commands with `--config asan` passed all 25 binaries and 311 cases, with counts matching each Release row. The sanitizer receipts matched commit `26df853e5c5f923c46db0ce2d6a3d2788ce1eb03` and tree `bbf45dc3f606414937c840f221ac40bbb1c46e93`. Only documentation changed between the Release and sanitizer receipts. All 18 receipts recorded unchanged inputs and the same source SHA-256, `af2912b62106e097416692a000f371ddcf3477dddfb048877657f5ab8aad95f1`.

## Sealed launcher

`bash cpp/gateway/bench/codex_real/sqlite.sh python3 -B cpp/gateway/bench/codex_real/build.py --receipt /tmp/uos-gateway-build/clang++-7053fde9eaca/release/receipt.json --out /var/tmp/cppgw-follower-live-fixture/sealed-final` assembled and bound 60 files from the Release `sqlite` receipt at the tested code commit. The manifest SHA-256 was `938dc5fa94f38abf1b5a18529a54a3e89cc74324b1d4c49e30fcce9a66980efb` and the gateway executable SHA-256 was `85c895a57bca52ef53b09ce42cf7083ea7c9b9df8ee4784ca5aaaf58276f8541`.

The local `launch-smoke` fixture ran via `cpp/gateway/bench/codex_real/sqlite.sh`, `sudo -n unshare --net --mount --pid --fork --mount-proc --propagation private --kill-child=SIGKILL` and `cpp/gateway/cmake/ci/isolate.py`. The outside-route probe returned `ENETUNREACH`. The sealed launcher refused the leader setting without follower mode and refused both a relative path and the follower's own state. The actual gateway adopted one login before listening and recovered a 401 after a second rotation with the same gateway process. The mock saw two usage GETs and zero OAuth calls. The gateway reported Landlock confinement and drained on SIGTERM.

## Committed-diff review

Both refs were fetched. Native Codex 0.171.0 reviewed base `fe308a3435a58cd2a0462a3117f54cdce80d3b97` and head `31a78c546f85631ef48f964811d0126a82da7a62` using the runner's configured model `gpt-6.1-sol` and exited 0.

```sh
codex review --base origin/claude/cpp-gateway-port -c 'model="gpt-6.1-sol"' \
  -c 'sandbox_mode="read-only"' -c 'shell_environment_policy.inherit="none"' \
  -c 'shell_environment_policy.set={PATH="/usr/bin:/bin"}'
```

> No introduced defects were identified with sufficient confidence to warrant an actionable finding. The changes preserve local credentials when leader reads or validation fail and coordinate live adoption with routing and shutdown. Tests were not executed in this read-only environment.
