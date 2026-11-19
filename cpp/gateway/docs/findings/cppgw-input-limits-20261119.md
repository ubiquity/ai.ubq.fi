# Native gateway input limits

Base branch `agent-jobs/cppgw-combine-1-20261119-44443689630-1`, commit
`e97ed985ef6c39377783e0263b10d15767de1432`, tree `95f338db7da4f5a6afc5a69c1d947c242350bfce`.

## Work and verification

- Read the native gateway README, CI driver guidance and porting rules. Inspected the clean checkout and started locating library call sites.
- `uname -m`, `cat /etc/os-release`, `g++-14 --version`, `pkg-config --modversion sqlite3 icu-uc`, and `bun --version` identified this host as Ubuntu 24.04.5 ARM64 with GCC 14.2, SQLite 3.45.1, ICU 74.2 and Bun 1.4.0. The qualified dependencies and isolated test runs are pending.
- The boundary inventory, code changes, regression tests and measurements are pending. No test results are claimed at this point.
- Added encoded and decoded byte limits to the strict and Buffer base64 adapters. The encoded ceiling is the padded size of the existing 50 MiB image allowance. The safe decoder receives an output buffer capped at 50 MiB. The test covers both limits independently, including an extra decoded byte with the same encoded size.
- Bounded request bodies at the HTTP connection, including chunk declarations before payload reaches llhttp. Kept 16,408 bytes and 198 fields for server heads and trailers, the 4 KiB chunk metadata line and the larger image-edit body allowances. Added parser cases for the exact limits and one beyond.
- Applied the shared head and body limits in the response parser used by Fetch and WebSocket handshakes. Fixed-length bodies and chunk declarations are checked before payload, close-delimited responses stop at the byte limit, and chunk lines and trailers have separate allowances. Added split-head and exact-boundary parser cases.
- Added cumulative compressed and decoded byte budgets and an expansion ratio to the gzip, Brotli and Zstd paths. Fetch keeps a 16 MiB ceiling and stored gzip keeps room for replay envelopes at 64 MiB. Zstd limits its decoding window to 16 MiB. Client HTTP content encodings reach the application as raw request bytes under the existing behavior. Added budget, gzip and Brotli checks to the existing library fixture.
