# Delivered native gateway on Apple Silicon

The qualification started at `0c54578151b88da9afe6d014007dbffe1176214b`, tree
`914c34ba615cd5de878ba8454595980668f3619d`, on `claude/cpp-gateway-port`.
The runner uses macOS 26.7.2 (25G332), arm64, Xcode 26.6 and Apple Clang 21.0.0 with the macOS 26.5 SDK.

## Checklist

- [x] Install the Homebrew dependencies in `docs/MACOS.md` and build the checksum-verified SQLite 3.54.0.
- [x] Check the C++ test registration with `python3 -B cpp/gateway/tools/links.py --check`.
- [ ] Build the delivered source and pass the isolated runtime suite.
- [ ] Run the whole registered suite and record binary and case counts.
- [ ] Run the release workflow's macOS packager and check the extracted version and health identity.
- [ ] Record the final source identity, receipts and any remaining failures.

The clean verification worktree is at `/private/tmp/cppgw-macos-final-20261119.2vuBKt/source`.
The sibling `build` directory contains compile and test receipts. `deps.log` and `sqlite.log` finished with exit 0.
The three-worker runtime run is in `runtime.log`; the detached launcher writes `runtime.exit` when it finishes.
Use `git log` and this checklist before resuming work.
