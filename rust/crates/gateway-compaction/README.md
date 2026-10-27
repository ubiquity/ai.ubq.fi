# Gateway compaction

Rust port of the existing gateway Jev compaction algorithm, originally adapted from `0x4007/fast-jev-compaction` commit `c1eab5fdbd6bde7d67f2da070116496558836884` under MIT; the upstream license is retained verbatim.

The algorithm source of truth is this repository's `lib/jev_compaction/{types,request,state,compact,codex_items}.ts` and `src/jev_compaction/{compaction,hybrid}.ts`, including its later fitting, bounded provenance, windowing and residue changes. M31's source contract and original source hashes accompany the focused golden fixtures.

The finite `CompactionHost` receives only Jev state/questions or the fixed buffered residue body, never a caller URL or credential. Its Jev transport must enforce the supplied 30000ms timeout through body consumption and forward caller cancellation; residue calls remain buffered with the source 20000ms bound. M05 owns interception, existing switch defaults and the original request/admission/fallback lifecycle.

The source-golden corpus covers exact header and response bytes, UTF-16 and JavaScript number/property semantics, fitting stages, windowing, dropped provenance, and partial residue behavior. Supplemental summary fixtures losslessly pack the actual source output, including cap fitting, stable equal-score ordering, accepted and refused multi-megabyte floors, and fitting before the no-reduction check. Controlled pending-I/O tests exercise the three-window and four-residue worker bounds plus cancellation.

Status: source implementation only. Native/WASM acceptance is bound to the matching immutable receipt, not this document. Actual Worker/runtime, admission/delivery and provider acceptance remain separate and are not claimed by the crate.

## Source-compatible compaction C1

The original Jev compaction implementation is partitioned by its defining responsibility:

- `gateway-compaction-source`: immutable UTF-16 text, data models, finite host trait and pure source helpers.
- `gateway-compaction-history`: transcript parsing/redaction and the complete history/state fitting ladder.
- `gateway-compaction-render`: verbatim rendering, identifier/chunk/body projections and opaque summary fitting.
- `gateway-compaction`: actual async decisions, batching, window/residue concurrency and local response entrypoints.
- `gateway-compaction-test-support`: dev-only original fixture loaders, recipes and synthetic host counters.

Consumers import definitions directly. The calibrated estimator remains solely in
`gateway_prompt_policy::tokens::estimate_tokens`; each estimator consumer depends on it directly.
`Text` retains its private shared UTF-16 buffer. The renderer's `Fit` has private fields, is created only by
fitting and offers consuming plain-data decomposition. The runtime builder accepts the original body,
finite host and response options; it never accepts an externally supplied fit.

This is a mechanical capability extraction from unaccepted P1
`b8f203ed2a6f4abe6621f406cd1468ee99f6eba6` plus ancestry-merged M60
`cb24be5e1ee79f53c23ae0e687956ba7860acabb`, based on public
`b59243d674567d418d1410f8390fc7fe47a08c92`. Original MIT/source provenance, the thirteen
compaction test cases and three fixture byte streams are retained. Tests live with their defining owners;
the support crate is never a production dependency. P1's nine estimator goldens read the same moved data.

Worker callers retain their actual predicate/build calls and original child/deadline/body-EOF ownership.
Linux x86_64 and direct WASM Cargo evidence is supplementary; canonical ARM64/confinement, controlled
workerd, Mac client and final production acceptance remain separate gates. No performance benefit is claimed.
