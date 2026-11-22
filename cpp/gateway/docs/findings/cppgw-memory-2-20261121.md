# Native gateway memory with the real Codex client

## Checklist

- [x] Identify the integration base and qualify the runner and confinement.
- [x] Generate realistic state and validate single, tool and resumed sessions through the packaged gateway.
- [ ] Record baseline samples at 1, 10, 100 and 500 sessions, allocation sites and the trim control.
- [ ] Publish resident, allocator and cache counters in health and request terminals.
- [ ] Measure each production change against the baseline and run the 500-session endpoint.
- [ ] Enforce a 20-session CI memory budget and run the change-scoped Release, ASan and registration gates.

## Starting point

The integration base is `707f7182b9a4826650f45f8e0f6e41095dd32974` with tree `b6fdaf36ab238f7ab111ae35b88043602cf97982`. It includes the first investigation's catalog changes. The runner is Ubuntu 26.04.1 on Linux ARM64 with four CPUs. The native, statically linked Codex CLI is 0.171.0. The installed launcher wraps that binary in JavaScript. The fixture invokes the ELF directly.

The workflow's SQLite 3.54.0 archive passed the pinned SHA-256 check. It was built with `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j4` and `sudo make install`. Noninteractive Linux network, mount and PID namespaces are available. [CI prerequisites](../CI.md)

## Harness qualification

Codex 0.171.0 needed its `api_key_model_discovery` feature to consume the custom provider's remote catalog. It sends the catalog instructions as an input message and exposes `exec_command` for shell calls. The fixture requires those instructions and all ten tool results at the upstream and checks that the result of a resume matches the programmed reply. A five-session development trial completed 15 inference requests at up to 417,130 upstream bytes. After the first tool session it measured 17,600 KiB anonymous and 54,788 KiB peak resident. That trial used the same integration base and a 377,511-byte draft catalog. The measurement fixture uses 389,001 bytes.

The public models endpoint passed its schema check with the 15-model Codex source and synthetic subscription. Publishing 535 models through the auth endpoint reached the runtime configuration's existing 4 KiB cap. The real-client mix uses the separate exact-version catalog. The seeded database is freshly compacted by SQLite insertion and has less on-disk fragmentation than the deployment. [Harness](../../bench/codex_real/README.md)
