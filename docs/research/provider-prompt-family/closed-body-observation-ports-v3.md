# Closed error-body observation ports v3

This provisional source-only checkpoint implements Root's explicit 2026-10-27T22:37 eleven-observation approval on adc9c599ba8c2ff6d77aa1f5539096d06f9b79cc/tree 6f794e717b1730c7ff7cbe98d3ef84cd3b539d1b. It preserves the existing branch, real bc8/input ancestry, producer/carrier checkpoints and protected evidence. Final real head/tree/parent are bound by PRESERVE_READY. Requested settings are gpt-6.1-sol/max; actual model/effort metadata available to D is unknown. This is neither compile/runtime acceptance nor whole-family completion.

## Exact eleven ports

The adjacent JSON supplies every current file hash and exact symbol line, full test/fixture/postimage inventory, dependency map, and unexecuted future gate argv.

| Defining consumer | Before -> after | Original phase retained |
| --- | --- | --- |
| rust/crates/gateway-claude-usage-admin/src/readback.rs | subscription_block(AuthResponseBody)->Result<SourceJson,GatewayError>; exhaustive Empty/Json/SourceJson -> Same signature; ClosedError checked into SourceJson then same private source-object projection, preserving duplicate-last and its existing missing claude_code null behavior | Admin readback DATA projection at original Result boundary; no dispatch/collector/auth/quota effect |
| rust/crates/gateway-worker/src/extras.rs | Rejected(AuthResponse,Vec<u8>); source_json(error.status,error.body().clone()); render tuple(SourceJson,u16,Headers) -> Actual defining AuthResponse path; total closed_error at same rejection; tuple(ExtrasResponseBody,u16,Headers); original direct body.as_bytes renderer | Parse rejection before original admission; render after same runtime/child completion, before physical delivery; finish/dispatch/settle_child/refund/observation body byte-identical |
| rust/crates/gateway-migration/tests/profile_runtime.rs | body(AuthResponse)->Value; SourceJson expect; Empty panic -> body->Result<Value,Box<dyn Error>>; finite ClosedError checked bytes; prior scalar parse failure/unexpected Empty remain explicit test failure; ? through three original test callers only | TEST observation after actual fixture operation; no production import/adopt/revision/CAS/lifetime policy changed |
| rust/crates/gateway-extras-runtime/tests/runtime.rs | scalar_source(&SourceJson)->Value; queued_job->tuple -> Shared private-test-module scalar_source(&ExtrasResponseBody)->Result<Value,Box<dyn Error>>; queued_job->Result<tuple,Box<dyn Error>>; ? only through traced tests/helpers | TEST observation after same runtime result; all original dispatch/cache/job/crypto/refund/settlement assertions retained |
| rust/crates/gateway-extras-runtime/tests/decisions.rs | result.body.deserialize::<Value>()? -> Explicit checked serde_json::from_slice::<Value>(result.body.as_bytes())?; upstream prepared-call SourceJson reads unchanged | Existing Result-capable TEST observation after same handle call; fixed Decisions dispatch and upstream request assertions unchanged |
| rust/crates/gateway-worker/src/inference_local_lifecycle.rs | wrapper_error()->RoutingError using now-private-kind-inaccessible literal -> Same Self return; evaluate boxed original500/server_error/no-code, fixed plain body, default headers in original order; RoutingError::new(error,headers,source_body) selects only false ordinary kind | Existing wrapper-construction failure; no local selection/currentness/cancellation/cleanup/accounting operation change |
| rust/crates/gateway-schema/tests/source_request.rs | Deserialize fixed error envelope to Value for message/param assertions -> Typed immutable message/param observations, preserving original400/message/omission/text assertions | TEST-only source validation observations; syntax order and source-domain behavior unchanged |
| rust/crates/gateway-routing-runtime/tests/source_plan.rs | error_field(RoutingError,&str)->JsString16 via Json/member unwrap -> Same private helper->Result<JsString16,Box<dyn Error>> using explicit finite ClosedError message/param; ? in its three traced tests | TEST-only pretransport source-plan message/param observations; original provider/model/source ordering assertions unchanged |
| rust/crates/gateway-routing-runtime/tests/source_factory.rs | Json/error/message parsing inside original factory-failure case -> Typed finite ClosedError message; all original pool/reserved/committed assertions retained | TEST-only pretransport original-admission cleanup observation; one original lifetime/zero provider exposure unchanged |
| rust/crates/gateway-routing-runtime/tests/source_contracts.rs | Json/error/param member observation -> Finite ClosedError param observation at same point; original captured route selection/attempt/completion assertions retained | TEST-only source-plan error before captured-selection work; actual target1460->1459 and no structural extension claimed |
| rust/crates/gateway-routing-runtime/tests/staged_claude.rs | matches!(source_body,Some(Json(_))) for locally constructed source error -> Explicit ClosedError match; all original status/reservation/attempt/body/usage and publication assertions retained | TEST-only staged Claude preparation rejection; sealed phase/capability/fallback semantics unchanged |

Claude's public subscription_block signature is unchanged. SourceJson and checked ClosedError reuse one private source-object projection. Its original last-property selection, object parse error and existing missing-block null construction are retained; no new null/error arm, collector or credential policy exists. The two pre-existing literal null constructions remain exactly two.

Worker Extras' complete run/completed-result/completion operation region is byte-identical. Parse rejection stays before original admission using a total finite AuthResponse. Rendering stays after existing runtime/child completion and before physical delivery; the full renderer body after tuple destructure is byte-identical. FinishHook, settle_child, dispatch, quota/refund/settlement, headers and observation/capture logic are unchanged. The wrapper constructor evaluates original boxed500/server_error/no-code, then the original fixed plain Internal Server Error body, then default headers, before the ordinary RoutingError::new(error,headers,source_body); only the private false kind can result. LocalLifecycle and response_error are byte-identical. No postterminal inference failure/refund/receipt or logging path was added.

All total producer/carrier Self APIs, private immutable wire snapshots, supplied untrimmed type, code omission, param omission/null/text and exact UTF16/bytes/MIME remain frozen. Generic SourceJson/serde operations remain fallible. Only private TEST observations gain Result; failure reaches their traced test callers, never production HTTP/SSE behavior. Migration Empty remains an explicit test failure, not a default/null/success. The scalar test view deliberately rejects source-valid non-scalar UTF16 rather than losing units.

## Test closure and preservation proof

Extras runtime994 would grow under checked observation propagation. Six complete existing SystemOne/image/provider-origin cases move together into runtime_ancillary.rs. runtime_support/mod.rs retains the complete actual fixture implementation, counted in both target closures. Its ExtrasHost implementation is byte-identical; only parent-test visibility and stable existing defining imports/scalar PrincipalId::from_scalar DATA spelling change. The fixture RNG remains private. No production helper, generic callback, code splicing or future Core8c5 facade is introduced.

| Target | Owned physical Rust lines including conventional modules |
| --- | ---: |
| rust/crates/gateway-extras-runtime/tests/decisions.rs | 305 |
| rust/crates/gateway-extras-runtime/tests/runtime.rs | 844 |
| rust/crates/gateway-extras-runtime/tests/runtime_ancillary.rs | 502 |
| rust/crates/gateway-migration/tests/profile_runtime.rs | 340 |
| rust/crates/gateway-routing-runtime/tests/source_contracts.rs | 1459 |
| rust/crates/gateway-routing-runtime/tests/source_factory.rs | 277 |
| rust/crates/gateway-routing-runtime/tests/source_plan.rs | 201 |
| rust/crates/gateway-routing-runtime/tests/staged_claude.rs | 615 |
| rust/crates/gateway-schema/tests/source_request.rs | 102 |

The shared fixture is279 lines. The retained Extras target closure is844 and ancillary closure502, each under1000. Existing source_contracts decreases1460->1459 and remains explicit inherited structural debt; no full Schema/Routing extension or baseline increase is claimed. The source-module scan is not the required official AST gate.

All88 original affected test names and explicit assertion counts remain; three additional observation cases are authored, not run:

- rust/crates/gateway-claude-usage-admin/src/readback.rs :: closed_error_readback_uses_the_existing_source_object_projection — NOT RUN.
- rust/crates/gateway-migration/tests/profile_runtime.rs :: finite_error_test_view_keeps_null_param_and_rejects_empty_body — NOT RUN.
- rust/crates/gateway-extras-runtime/tests/runtime_ancillary.rs :: finite_response_test_view_keeps_wire_and_rejects_nonscalar_json — NOT RUN.

The three vectors cover finite Claude readback versus identical validated source JSON without a new missing-block rule; scalar/error/null-param/Empty test observation; and direct finite UTF16 wire/MIME preservation plus actual scalar-JSON parse rejection. The existing wrapper case additionally checks false private overflow identity. All109 provider originals, fifteen regressions, local-refusal original/new cases, all eleven carrier cases and five original provider fixtures remain untouched. Twenty-one relevant fixture files are hash-identical. Old executed receipts are not current evidence.

Only Claude-usage-admin production grows,338->366 including its inline tests, within1000. Worker production remains18540, Schema7494 and Routing7867; no oversized production owner grows. All twenty provider/P1/refusal owners remain bounded, maximum841. Complete production and target inventories are in the JSON; remaining global structural debt stays with its existing owners.

## Mechanical and relocation delivery

Five first-party package dependency/lock records change only for actual stable defining imports/test consumers. All312 actual external Cargo.lock records, versions/sources/checksums/dependency arrays, workspace features/profiles, root Cargo and quality baseline remain unchanged. Source primitives and single encoder are unchanged. Manifest-source normal first-party dependencies remain acyclic; the refusal/source foundation gains no auth/Worker/Authority reverse edge.

The separately hashed mechanical-imports-and-cargo.patch and manifest contain candidate-bound defining imports and local edges; it must compose with the complete feature patch, including the conventional test module/cohort move. It is not a standalone qualification view. relocation.patch carries exact staged new-file/source changes and hashes. Root/actual AC/EX/Claude/Worker/Migration/Routing owners must relocate by function/signature/base hash onto their newer physical sources. No 7694/new Native/WF branch was fetched, merged, copied or overwritten. Future8c5 Core types were not adopted or guessed; stable current C1073 source primitives and scalar DATA constructors are used directly.

## Source observations and pending gates

One pinned formatter mutation pass covers only thirteen exact changed Rust files, repository config and skip_children=true. Final exact-file check and git diff --check return0. A formatter stdout preview caught an import-before-inner-doc-comment typo; it was corrected before the mutation pass, and the failed preview remains evidence. No tests, compiler/rustc/Clippy/build, gateway-quality driver, install, provider/service/credential/private-state/CI or compiler-control/target mutation occurred. No owned jobs or children remain.

The JSON focused-gates-plan is PLAN ONLY. Every future compiler/test-driver command first sources the exact D environment and uses lease-exec.py D under a separately granted legitimate lease and source-bound target. It lists parser fixtures/structural check, selected native/WASM compiler and strict six-lint Clippy closures, unchanged strict comparator calls with complete Cargo JSON/current parser contexts, and only the named affected tests with serial nonzero execution requirements. Actual Linux x86_64 source evidence is separate from canonical Mac/real native confinement and Worker/client acceptance. Unsupported architecture, fixture OS/tooling or confinement fails closed; no spoof, fallback facade or gate change is proposed. No comparator updater or baseline change is planned.

Other seven policy interfaces, native aggregate stream rendering, actual sixteen-case workerd/native-client/Mac acceptance, shipping/isolated-fixture artifacts and full-family qualification remain held. Current canonical Core integration and newer owner relocation are separate prerequisites; unleased nominal/authority consumers may still block compilation. No semantic proof is inferred from this source checkpoint. Source freezes at PRESERVE_READY pending independent LOCAL_PRESERVED; the full provider/P1 and Rust goal remain unfinished.
