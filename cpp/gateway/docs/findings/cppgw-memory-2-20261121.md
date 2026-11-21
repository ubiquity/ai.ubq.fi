# Native gateway memory with the real Codex client

## Checklist

- [x] Identify the integration base and qualify the runner and confinement.
- [ ] Generate realistic state and validate single, tool and resumed sessions through the packaged gateway.
- [ ] Record baseline samples at 1, 10, 100 and 500 sessions, allocation sites and the trim control.
- [ ] Publish resident, allocator and cache counters in health and request terminals.
- [ ] Measure each production change against the baseline and run the 500-session endpoint.
- [ ] Enforce a 20-session CI memory budget and run the change-scoped Release, ASan and registration gates.

## Starting point

The integration base is `707f7182b9a4826650f45f8e0f6e41095dd32974` with tree `b6fdaf36ab238f7ab111ae35b88043602cf97982`. It includes the first investigation's catalog changes. The runner is Ubuntu 26.04.1 on Linux ARM64 with four CPUs. The native, statically linked Codex CLI is 0.171.0. The installed launcher wraps that binary in JavaScript. The fixture invokes the ELF directly.

The workflow's SQLite 3.54.0 archive passed the pinned SHA-256 check. It was built with `./configure --prefix=/usr/local --disable-static --disable-readline CFLAGS='-O2 -DSQLITE_DQS=3'`, `make -j4` and `sudo make install`. Noninteractive Linux network, mount and PID namespaces are available. [CI prerequisites](../CI.md)
