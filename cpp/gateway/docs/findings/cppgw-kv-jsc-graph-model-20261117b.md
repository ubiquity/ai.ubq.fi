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
