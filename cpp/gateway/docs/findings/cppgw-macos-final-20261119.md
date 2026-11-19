# Delivered native gateway on Apple Silicon

The qualification started at `0c54578151b88da9afe6d014007dbffe1176214b`, tree
`914c34ba615cd5de878ba8454595980668f3619d`, on `claude/cpp-gateway-port`.
The runner uses macOS 26.7.2 (25G332), arm64, Xcode 26.6 and Apple Clang 21.0.0 with the macOS 26.5 SDK.

## Checklist

- [x] Install the Homebrew dependencies in `docs/MACOS.md` and build the checksum-verified SQLite 3.54.0.
- [x] Check all 450 registrations with `python3 -B cpp/gateway/tools/links.py --check`.
- [x] Build all 368 production sources, link the arm64 gateway and pass 37 runtime binaries and 524 cases.
- [ ] Run the whole registered suite and record binary and case counts.
- [ ] Run the release workflow's macOS packager and check the extracted version and health identity.
- [ ] Record the final source identity, receipts and any remaining failures.

The clean verification worktree is at `/private/tmp/cppgw-macos-final-20261119.2vuBKt/source`.
The sibling `build` directory contains compile and test receipts. `deps.log`, `sqlite.log` and `runtime.log`
finished with exit 0. The detached launcher writes each job's exit status beside its log. The full suite
started after the runtime pass and writes to `full.log`. `package.log` is queued behind the full suite.
`package-health.log` follows the packaging build receipt to check the health handler offline.

## Runtime checkpoint

Both suites use three jobs and separate empty `HOME` and `TMPDIR` directories under the scratch root.
The runtime command was `bash cpp/gateway/tools/macos-build.sh --build-root "$scratch/build"
--sqlite-prefix "$scratch/sqlite-3.54.0" --jobs 3 --suite rt --require-clean` under `env -i`
with `PATH=/opt/homebrew/bin:/usr/bin:/bin`. The full command uses `--suite all` with the same build root.
Seatbelt accepted loopback and denied the external connection during preflight.

The runtime receipt at `build/runs/1795130862276180000` records a clean, unchanged source identity and the
unchanged inputs. The SHA-256 values for `provenance.json` and `results.json` are
`d9ea1350d1472e222a094ccd8f15e046e4b2ac9a06c6deabd6a129e9667428b7` and
`9a52c0e2b05a8fa207c5ccb4cbb83a8f8b9c0a268a8edc28d98bb05d84358a76`, respectively.

## Gemini fixture path

A focused reproduction using the driver's link inputs and Seatbelt profile failed on the delivered source.
The first assertion failure in `gemini-reproduction.log` was:

```text
tests/gemini_subscription_test.cpp:276: expected captured.find("TMPDIR=" + root + "/.data/antigravity-run-") != std::string::npos
FAIL Gemini CLI receives only the minimal environment required for the owner CLI
1 run, 0 passed, 1 failed
```

The fixture creates its root through `/tmp`. Darwin's `getcwd` returns `/private/tmp`, so the real child
workspace has that prefix. The test now derives its expected data directory with `realpathSync` on Darwin
and checks the same `antigravity-run-` path prefix. The Linux expectation continues to use the original path.
