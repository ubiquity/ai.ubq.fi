# C++ gateway release workflow findings

## Work contract v1

Build downloadable Linux ARM64 and Apple Silicon packages for `workflow_dispatch` and `cpp-gateway-v*` tags. Linux uses the Bookworm portable build, with Debian 12 and 13 container proofs. Arch Linux ARM and Raspberry Pi OS use an offline seal against the target's libraries and retain the existing Linux kernel requirements. macOS uses `tools/macos-build.sh --suite rt` and the existing entrypoints. Darwin production startup currently fails closed and remains outside the owned release files.

Owned files are `.github/workflows/cpp-gateway-release.yml`, `cpp/gateway/packaging/`, `cpp/gateway/docs/RELEASES.md` and this report. Preserve the application, tests, authentication, accounting, storage and OS confinement. Leave live state, service activation and provider traffic outside this work. Use an empty test home, fresh scratch and credential-free local mocks.

Acceptance requires byte-identical fresh Linux archives from the same commit, the same Debian proof command as the workflow, `actionlint`, and install and rollback instructions for all three targets. Record the commit and tree, exact commands, counts, timings, and any untested targets. Discovery still needs to confirm the runner's kernel support and the Mac link and library requirements. The macOS job is unexecuted on this Linux runner.
