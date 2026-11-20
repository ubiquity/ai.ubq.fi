# Delivered native gateway on Apple Silicon

The qualification started at `0c54578151b88da9afe6d014007dbffe1176214b`, tree
`914c34ba615cd5de878ba8454595980668f3619d`, on `claude/cpp-gateway-port`.
The runner uses macOS 26.7.2 (25G332), arm64, Xcode 26.6 and Apple Clang 21.0.0 with the macOS 26.5 SDK.

## Checklist

- [x] Install the Homebrew dependencies in `docs/MACOS.md` and build the checksum-verified SQLite 3.54.0.
- [x] Check all 450 registrations with `python3 -B cpp/gateway/tools/links.py --check`.
- [x] Build all 368 production sources, link the arm64 gateway and pass 37 runtime binaries and 524 cases.
- [x] Pass the whole registered suite on the fixed commit and record binary and case counts.
- [x] Build the release workflow's macOS archive and check the extracted version and health identity.
- [x] Record the final source identity and receipts. All build, test and package gates passed.

The clean verification worktree is at `/private/tmp/cppgw-macos-final-20261119.2vuBKt/source`.
The sibling `build` directory contains compile and test receipts. The detached launcher writes each job's exit
status beside its log. `runtime-final.log`, `full-final.log` and `package-final.log` run in that order from
commit `99c2969bf83d883a01f1858d61319bbd656c19d6`, tree `56db4224b562e0795b2240c7a36256862dabc3d5`.
`verify-final-retry.log` checks the extracted archive and `package-health-final.log` checks the offline health handler
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

## Final native tests

The standalone runtime gate and full suite both exited 0 from clean commit
`99c2969bf83d883a01f1858d61319bbd656c19d6`, tree `56db4224b562e0795b2240c7a36256862dabc3d5`.
The driver compiled all 368 production sources and 471 test and support sources, archived all six vendors,
and linked the arm64 gateway. The test roster matched the 450 entries from `links.py --check`.

| Gate | Passing binaries | Passing cases |
| --- | ---: | ---: |
| Standalone runtime | 37/37 | 524/524 |
| Full suite, runtime | 37/37 | 524/524 |
| Full suite, application | 413/413 | 4,680/4,680 |
| Full suite, total | 450/450 | 5,204/5,204 |

| Receipt under `build/runs` | `provenance.json` SHA-256 | `results.json` SHA-256 |
| --- | --- | --- |
| `1795132384072967000` | `1e36d133417e5d075be699f19258f6340a03493cf7540906f983685c39e9d012` | `40371a2db49ec71fef97c56e56c3f77dad27a96a9fc9e92e685cff99c7ae196b` |
| `1795132546666757000` | `8492d08805138f738de504595cbcd2a431aa6ddb1829e68912ae4ed4948a3196` | `0be86e902ea961aa0566c6b8e80baaed64a0612570c9fc20258ef251d00eef32` |

## Release package and health identity

After the full suite passed, the release job's command ran from the same clean commit with separate empty
home and temporary directories.

```sh
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin HOME="$scratch/package-final-home" TMPDIR="$scratch/package-final-tmp" \
  python3 -B cpp/gateway/packaging/macos/build.py "$scratch/release/uos-gateway-macos-arm64.tar.gz"
```

The packager exited 0. Its release-commit build passed 37/37 runtime binaries and 524/524 cases, and the arm64
executable passed the packager's system dylib and code-signature checks. Run `1795133292623980000` used
`-DUOS_PACKAGE_GIT_COMMIT="99c2969bf83d883a01f1858d61319bbd656c19d6"`. Its retained `provenance.json` and
`runtime-results.json` are under `packaged-health-final/` with SHA-256 values
`1431f6835e8c3434e31a153bd298bf2e37604f75646f44dd6c15709e1b1a776c` and
`3e974b2f78425a8b7f9cad4f43da09c1f1a04da5a07190ae84fe95115d0009c1`, respectively.

`shasum -a 256 -c SHA256SUMS` passed for the 47,748,175-byte archive. A fresh extraction's `VERSION` matched
the source commit and tree and records macOS 26.0 as the minimum. Under Seatbelt with networking denied,
`./bin/uos-gateway --version` printed `uos-gateway 99c2969bf83d883a01f1858d61319bbd656c19d6`.

| Artifact | SHA-256 |
| --- | --- |
| `release/uos-gateway-macos-arm64.tar.gz` | `4736a724e1114968d27e6434fed7a6f2a713e88ed2ed498ea66018f3075c1361` |
| Extracted `bin/uos-gateway` | `c091ae502d5e382408f961f37c4db56220828dc24c51006eeba3dd06e1023c90` |
| Package build's `libuos_gateway.a` | `d6b84a2ea558209ca3dc0e8f36020d6835282148cb616195431b31710a2fa84a` |

The health proof is offline because serving uses the sealed Linux startup path. A probe linked against the
packager's verified application and vendor archives called `uos::health::handleHealth()` under network-denying
Seatbelt. It returned HTTP 200 with the following identity in both the JSON body and response headers.

| Body field | Header | Value |
| --- | --- | --- |
| `release.git_sha` | `x-uos-git-sha` | `99c2969bf83d883a01f1858d61319bbd656c19d6` |
| `release.deployment_id` | `x-uos-deployment-id` | `mac-99c2969bf83d883a01f1858d61319bbd656c19d6` |

`xcrun nm -nm` and `xcrun dyld_info -fixups` located the release string used by the health handler in the
extracted Mach-O. Reading the `uos::release::RELEASE_GIT_SHA` string view from the executable resolved to the
same 40-character commit. The first inspection script accepted lowercase addresses only and raised
`ValueError: not enough values to unpack (expected 1, got 0)` on `dyld_info`'s uppercase addresses. The corrected
inspection passed on a fresh extraction of the same archive.

The commits after the verified SHA contain qualification documentation only. `git diff --check` passed.
