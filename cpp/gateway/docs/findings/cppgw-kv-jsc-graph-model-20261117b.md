# Sparse and cyclic stored values

## Work contract v1

Repair the C++ JavaScriptCore stored-value reader and typed-value interchange
for sparse arrays, shared nodes, and cycles. The replacement plan keeps the
existing codec because it has no standalone library implementation. This lane
owns only the value model, JSC codec, affected interchange/conversion code,
component tests, and this report. The TypeScript oracle, wire formats, committed
fixtures, deployment configuration, and production state are outside the edit
scope.

| Surface | Required behavior |
| --- | --- |
| Ordinary values | Existing Bun vectors, error semantics, and codec performance |
| Sparse arrays | Bun's length, own indices, and undefined behavior with allocation based on present entries |
| Object graph | Preserve identity across JSC, typed interchange, and both offline conversion directions |
| Offline fixture | Run the original forward/reverse proof against disposable, stopped data with pinned Deno |

The baseline is `55afcc8b13b2ddd10747355745da19e5264f7b76`. Bun 1.4.0
deserializes `05000000012cfbff00ffffffff` to an array of length 16,775,980
with no own entries. Two existing C++ tests encode the known graph
divergences. The stored-value test already records the Bun cycle result
alongside the C++ error. The typed-value differential already records Bun's
reference identities. The repair will exercise those Bun expectations and
retain the recorded literals.

The selected gates are the strict port-flag build, original and added KV
vectors, the affected test link closures in release and ASan/UBSan, the
unchanged synthetic offline proof, and a matched codec benchmark. Build
objects, pinned dependencies, test HOME/TMPDIR, and proof data live outside
the checkout. The job closes when these gates finish, with any independent
runner or fixture blocker recorded against the exact attempted scope.

## Status

Toolchain and oracle setup started. No implementation or acceptance results yet.

## Continuation at `a705abda`

The inherited commit contains the graph model, the sparse reader and the first
13 sparse-array vectors. The strict Clang 21.1.8 release build passed the
three focused binaries (17 cases). This run included the existing 86-row
serialized corpus and the typed interchange differential. The portable runner
needed `--system-header-prefix=third_party/` for Clang to treat the vendored
simdutf header as a system header, as the sanitizer driver already does.

The continued work uses contract v1 with the same file and behavior scope.
The acceptance surface includes numeric keys in JSC's array property block.
Bun 1.4.0 accepts the valid index 4,294,967,293 in that block because its raw
index word is reserved as the block marker. The inherited reader rejects it
as a named property, and its writer would write the raw marker. The reader
and writer must handle that index while retaining the existing named-property
refusal. Bun observations used the unchanged TypeScript `encodeTypedValue`
to check length, ordered present entries, explicit undefined and a hole read.
All 13 inherited sparse vectors matched. The numeric property-block probe
adds coverage beyond those vectors.

The test closures all include `src/rt`; the release and sanitizer sweeps
therefore select the full standalone registry. The separate synthetic proof
uses the pinned Deno 2.9.7 ARM64 binary with SHA-256
`5ecc9a6b862d61d4cf8cae3ac5f29488ffc839d675cfd60c11d908121dfe5bf1`.
Both runs use isolated scratch state with explicit, provider-free environments.
The benchmark compares the original and current trees on the committed KV
fixture inputs. Compilation reuses objects per profile.
