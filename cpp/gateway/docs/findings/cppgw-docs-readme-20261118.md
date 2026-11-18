# Native gateway owner guide verification, 2026-11-18

## Work contract v1

Write the owner entry point and architecture guide for the C++20 gateway at starting commit
`33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Own `cpp/gateway/README.md`, `cpp/gateway/docs/ARCHITECTURE.md`,
and this report. Preserve all other tracked files, including the TypeScript gateway, C++ application and fixtures,
existing documents, and workflows. Use the source for current behavior and exact-build receipts for measured results.

The platform matrix is Linux ARM64 for release and production confinement, Linux x86_64 for portable build and local
fixtures, Apple Silicon for the recorded development suite, and Raspberry Pi for an unqualified target. Use local mocks
only. Production traffic, service activation, credential changes, and database conversion are outside this work.

Verify the README's commands with the portable Release driver and record the observed platform, compiler, host libraries,
counts, failures and launch outcome. The driver uses an external build directory and isolated Linux fixtures. The
namespace setup, installed library versions, and x86_64 startup outcome need local confirmation. The work ends with a
verified Markdown and relative-link check, `git diff --check`, and a complete findings record.

## Initial inventory

`bash cpp/gateway/tools/ci-build.sh --list | wc -l` returned `441` on Ubuntu 26.04.1 LTS, Linux x86_64.
The suite-repair report records 441 Release binaries and 5,031 cases passed on Linux ARM64 at the last source/test
commit, `380a9b0a19fca261c910967ee24078f1b22aec9a`. [Suite-repair receipts](cppgw-suite-repair-20261118.md)
