# C++ gateway stage 2a integration

## Work contract v1

Integrate the seven accepted branch tips in the requested order on the suite repair at
`33ea9b20d1f02b96bb86a24c0530641ccd4fd525`. Preserve wire formats, status codes, storage,
authentication, validation, resource limits, accounting, and real fixture confinement.
Keep the standard-library behavior accepted by each branch. Use Boost.Beast for the outbound
WebSocket operations it replaces, ICU for casing and collation, and simdutf for UTF conversion.
Terminal logging must preserve the existing record formats and request settlement.

The integration owns `cpp/gateway/` and the C++ gateway workflows. TypeScript, other C++ projects,
vendored bytes, production services, live data, and credentials are outside this stage. Tests use
local mocks, a sparse environment, and private temporary state. The macOS, Raspberry Pi, and
operator branches supply build and handoff artifacts. Physical host qualification is a separate step.

Acceptance requires all four portable release shards on Linux ARM64, focused ASan/UBSan coverage
of the merged components, CMake and vendor-registry checks, and a driver/CI receipt for the pinned
SQLite headers and loaded library. The merge graph must retain the seven incoming tips. The report
records the exact commands, tree, counts and unresolved items for the maintainer's next integration.

Discovery confirmed shared ICU ancestry on the logging, macOS, Pi, operator and audit lines. The
starting release has 33,588 physical lines in tracked `src/rt` and `src/runtime` files, counted with
`git ls-files -z cpp/gateway/src/rt cpp/gateway/src/runtime | xargs -0 wc -l`. Build availability
and merge conflicts will be established by the actual merges and runner.

## Merge order

| Order | Change | Incoming commit |
| ---: | --- | --- |
| 1 | Unified release, sanitizer, and robustness CI | `156a3b8e635aa02d2ffca8d9d7331cea5096e208` |
| 2 | Terminal log records | `436369dbdf7ab6ae2d65e4637ca2c0b5ffebc4a8` |
| 3 | Boost.Beast outbound WebSocket | `e030bdd61e1f8081cd64a6ceac8e9b3c76b64f09` |
| 4 | macOS requalification and ICU strings | `33f2cd3356e6a27a4daa111473ab1aff8633f63b` |
| 5 | Raspberry Pi packaging | `da56e2277e0b0ef6ee0da3b3f504dfea16069bd2` |
| 6 | Side-by-side operator kit | `bb167116d55d91259e7eb62b7dabdb55863309e0` |
| 7 | Runtime audit and briefs | `d0c8d551bbb6334300c930bda29eb4f8e4e93632` |
