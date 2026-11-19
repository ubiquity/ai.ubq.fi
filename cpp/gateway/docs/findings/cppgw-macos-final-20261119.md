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
The sibling `build` directory contains compile and test receipts. The detached launcher writes each job's exit
status beside its log. `runtime-final.log`, `full-final.log` and `package-final.log` run in that order from
commit `99c2969bf83d883a01f1858d61319bbd656c19d6`, tree `56db4224b562e0795b2240c7a36256862dabc3d5`.
`verify-final.log` checks the extracted archive and `package-health-final.log` checks the offline health handler
using the packaging inputs. Use this checklist and `git log` to resume.

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
`gemini-reproduction.log` recorded the assertion failure.

```text
tests/gemini_subscription_test.cpp:276: expected captured.find("TMPDIR=" + root + "/.data/antigravity-run-") != std::string::npos
FAIL Gemini CLI receives only the minimal environment required for the owner CLI
1 run, 0 passed, 1 failed
```

The fixture creates its root through `/tmp`. Darwin's `getcwd` returns `/private/tmp`, so the real child
workspace has that prefix. The test now derives its expected data directory with `realpathSync` on Darwin
and checks the same `antigravity-run-` path prefix. The Linux expectation continues to use the original path.
The corrected Gemini binary passed 27/27 cases in `gemini-fixed.log` after commit
`f2b1237c61ac8d7bae0537c7da4def3ac3efb1e5`. The first full attempt (`full.log`) compiled all 434 application
test and support sources. I interrupted it after compilation and reused its object cache for the clean rerun
with the fixture fix.

## Committed review attempt

The fetched base was `0c54578151b88da9afe6d014007dbffe1176214b` and the reviewed head was
`f2b1237c61ac8d7bae0537c7da4def3ac3efb1e5`. The native CLI ran this command with a fresh home and Seatbelt
network denial.

```sh
codex -c 'model="codex-auto-review"' -c 'review_model="codex-auto-review"' \
  -c 'sandbox_mode="read-only"' review --base 0c54578151b88da9afe6d014007dbffe1176214b
```

The banner reported `codex-auto-review`. The review timed out with exit 124 after `failed to connect to Responses
WebSocket stage=Unknown`, with no complete findings to ingest.
