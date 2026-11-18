# Slim C++ gateway environment and filesystem helpers

## Work contract v1

Base commit `6393434b110e2d1afe93e6c7005848219336d9b9`, tree
`89b42863ac7dd6a07610e2d15aa320efc42547af`. Retire the fixture-only dotenv
entrypoints first, then consolidate filesystem text conversion and lexical
path operations. Each brief gets a separate buildable commit.

| Surface | Required behavior |
| ------- | ----------------- |
| Environment | First-use OS snapshot, explicit empty values, enumeration, nested overrides, and inherited child environment |
| File text I/O | Shared UTF-8 codecs, retained BOM and NUL, replacement of malformed input, and existing descriptor offsets |
| Lexical paths | POSIX byte handling, distinct join and resolve semantics, and containment checks at the existing callers |

Owned paths are the assigned regions of `src/rt/env.cpp`, `include/uos/rt/env.hpp`,
`src/rt/fs.cpp` and `include/uos/rt/fs.hpp`, their runtime fixtures, an appendix
in `docs/DEVIATIONS-FROM-BUN.md` and this report. Deployment, credentials,
provider traffic, descriptor security, TLS, subprocess construction, and other
lanes' files are outside the contract. Gateway wire formats, accounting, stored
values, authentication and validation decisions are invariants.

Use `bash cpp/gateway/tools/ci-build.sh --config release` for the named
fixtures and the whole registry, and `--config asan` for every test linking an
edited runtime object. Each test runs in a credential-free loopback namespace
with isolated home and temporary directories. Record exact receipts and
physical production line counts. The runner is Linux ARM64. Apple Silicon
execution depends on a matching host and its qualified fixture boundary.

## Progress

### Environment

The gateway and both offline CLIs require explicit manifests. The draft
native unit uses a systemd `EnvironmentFile` and its launcher passes the
resulting OS environment to the executable. The deployed Bun launcher owns
its explicit repository-root dotenv policy. TLS and child environment
construction are outside the edited closure.

`git grep -nE '\\b(loadEnvFile|parseEnvFile)\\b' -- cpp/gateway` found only
the declarations and implementation, the ordinary env fixture, the existing
env fuzz fixture and documentation or metadata. The original snapshot and
lookups retain their implementation. Enumeration uses a stable sort and the
whole-view fixture uses a scoped swap. The
runtime fixture keeps all 124 original dotenv inputs and exercises explicit
OS precedence, present-but-empty values and nested restoration. The existing
fuzz target exercises snapshot and override restoration with its arbitrary
input bytes. Its descriptive `entry` field still names the retired parser
in the out-of-scope fuzz registry. Neither fuzz driver reads that field.

Environment production files went from **281 to 185 physical lines**
(**96 removed**). The four named release gates and combined suite are pending.
