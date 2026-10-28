# Closed error-body carrier API v3

This is a provisional source-only checkpoint under Root's 2026-10-27T21:54 named carrier approval, based on 1cad4dd8bf3bf99db517b8c136e572bbb5d5d94f/tree 837b0aafa82afe5093508ec44233a95b496cf356. It preserves real bc8 ancestry, the separate OwnedRequest field-label correction, all prior source repairs and protected handoff evidence. Actual checkpoint head/tree/parents are supplied by PRESERVE_READY; this document does not claim compiler, runtime or whole-family acceptance. Requested model/effort are gpt-6.1-sol/max; D has no trusted actual metadata fields.

## Finite API and wire contract

ClosedErrorBody keeps private immutable message: JsString16, error_type: String, code: Option<String>, param: Option<Option<JsString16>>, wire: String. Its original Self producers and their snapshot-before-cleanup timing are unchanged. This cut adds only Serialize -> Result<S::Ok,S::Error> through finite borrowed fields in message/type/code/param order. It reuses the defining JsString16 serde path; generic representation/serializer/I/O errors remain explicit at that trait boundary. Direct wire reads do not serialize or parse. No RawValue assertion, unchecked source constructor, copied encoder, arbitrary raw JSON constructor, Deserialize or new HTTP failure is introduced.

None code omits code; None param omits param; Some(None) is null; Some(Some(text)) retains exact units. The supplied wire type remains untrimmed. The original private overflow tag is neither serialized nor constructible through body/metadata/header input. All SourceRequestError constructors, routing cleanup, original headers/status and clones retain prior behavior.

AuthResponseBody retains Empty, Json(Value), SourceJson(SourceJson), adding ClosedError(ClosedErrorBody). AuthResponse::closed_error(status:u16,body:ClosedErrorBody)->Self uses the same JSON/no-store headers, empty separate-cookie list and stream=None as source_json. to_bytes still returns Result<Vec<u8>,GatewayError>, with direct immutable bytes for both source representations. Generic Serialize remains fallible; as_json still observes only Json(Value). Scalar error/json APIs are unchanged.

ExtrasResponse.body becomes the defining ExtrasResponseBody::Json(SourceJson) | ClosedError(ClosedErrorBody). as_str/as_bytes borrow the actual wire; equality remains exact byte equality. Generic Serialize delegates through the existing Result boundary without an enum tag. source_error and source_json remain total Self constructors using the same private data-only metadata assembly: JSON content type, semantic=None, usage=None, kernel=FromAggregate, failure_kind=None, empty upstream_errors/health. Existing scalar json/error and voyage behavior are unchanged. The new type does not expose SourceJson parsing conveniences to conceal required consumer ports.

Arbitrary validated SourceJson, scalar Value, Empty and the fixed plain Internal Server Error carrier remain distinct. Json and ClosedError use application/json; plain uses text/plain; charset=utf-8. Caller header overrides, repeated Set-Cookie appends and capture bytes keep their original ordering. No admission, dispatch, settlement, quota, receipt, refund or logging operation was added.

## Exact producer/consumer phases

The JSON map gives current file hashes, exact symbol lines, before/after signatures and all source inventories.

| Consumer | Result/phase preserved | Change |
| --- | --- | --- |
| rust/crates/gateway-local-refusal/src/closed_error_body.rs :: pub struct ClosedErrorBody, impl serde::Serialize, fn serialize | Serialize -> Result<S::Ok,S::Error>; all existing Self producers remain Self; Pure immutable DATA; no effects or delivery | Add finite borrowed Envelope/Fields Serialize; private constructors/wire/observations unchanged |
| rust/crates/gateway-auth-contracts/src/types.rs :: pub enum AuthResponseBody, pub fn as_json, pub fn to_bytes, impl serde::Serialize, pub fn closed_error, pub fn error | closed_error(u16,ClosedErrorBody)->Self; to_bytes()->Result<Vec<u8>,GatewayError>; Serialize->Result<S::Ok,S::Error>; Pure response DATA; scalar error/json/source_json/stream APIs retain behavior | Add ClosedError(ClosedErrorBody); direct bytes and total closed_error constructor; as_json stays Json-only; generic trait delegates fallibly |
| rust/crates/gateway-extras-contract/src/response_body.rs :: pub enum ExtrasResponseBody, pub fn as_str, pub fn as_bytes, impl PartialEq, impl serde::Serialize | as_str()->&str; as_bytes()->&[u8]; Serialize->Result<S::Ok,S::Error>; Pure DATA; no admission/dispatch/refund/receipt/settlement | ExtrasResponseBody::Json(SourceJson)  /  ClosedError(ClosedErrorBody); borrowed wire/bytes; byte equality; finite Serialize delegation |
| rust/crates/gateway-extras-contract/src/types.rs :: pub struct ExtrasResponse, pub fn source_error, pub fn source_json, fn with_body, pub fn json, pub fn error | source_error(SourceRequestError)->Self; source_json(u16,SourceJson)->Self; Pure carrier assembly; callers span pretransport errors and postdispatch/terminal outcomes; their bodies unchanged | source_error creates finite ClosedError; source_json creates Json; private with_body preserves exact metadata defaults; scalar json/error unchanged |
| rust/crates/gateway-schema/src/source_request/reasoning.rs :: pub fn parse_reasoning, let message = | Result<Option<SourceJson>,SourceRequestError>; Pretransport/presemantic syntax; no effects | body().message().clone(); same parent reasoning param and original order |
| rust/crates/gateway-catalog-runtime/src/admin.rs :: fn runtime_error, let message =, fn runtime_error_keeps_opaque_model_units_and_original_status_mapping | SourceRequestError -> SourceRequestError; Pure projection following failed runtime-config validation; no operation body edits | Typed message getter; exact code/byte test observations; same 413 too-large/4-KiB versus409 classification |
| rust/crates/gateway-catalog-provider/src/response.rs :: pub fn into_auth_response, AuthResponse::closed_error | Result<AdminCatalogResponse,SourceRequestError> -> AuthResponse; No-effect rendering after selected catalog operation; prior effects remain owned | closed_error for finite body; success source JSON/status/header extension unchanged |
| rust/crates/gateway-admin-runtime/src/defaults.rs :: fn validate_defaults_fields, fn defaults_runtime, let scalar_body =, AuthResponse::closed_error | validate_defaults_fields: Result<Option<AuthResponse>,GatewayError>; defaults_runtime: Result<ControlFlow<AuthResponse,(ModelId,JsString16,Option<SourceJson>)>,GatewayError>; Unknown-field validation before defaults reads/CAS; runtime/default conversion after same reads and before writes; scalar Err/opaque Break unchanged | closed_error with identical evaluation order; exact byte comparison after same diagnostic conversion |
| rust/crates/gateway-admin-runtime/src/state.rs :: async fn selection, AuthResponse::closed_error | Result<AuthResponse,GatewayError>; After authorization, before selection read/CAS; no effects moved | closed_error with same original status/body; ID order and early return unchanged |
| rust/crates/gateway-worker/src/inference_source.rs :: pub(crate) async fn error_response, body.content_type(), capture.immediate | Result<worker::Response,GatewayError>; Before body delivery; may follow dispatch/selected-response preparation/cleanup; real platform errors retained | Pure content_type observation for Json/ClosedError/plain; original bytes/status/no-store/header overrides then awaited capture |
| rust/crates/gateway-worker/src/inference_local_lifecycle.rs :: pub(crate) fn response_error, body.content_type(), pub(super) fn wrapper_error | Result<worker::Response,RoutingError>; After local selected QuotaReplacement choice, before wrapper/delivery; no cleanup/accounting change | Same-point content_type observation; wrapper error, lifetime and header ordering unchanged |
| rust/crates/gateway-worker/src/runtime.rs :: pub fn auth_response, response.body.to_bytes(), for cookie in response.set_cookies | Result<worker::Response,GatewayError>; After selected operation before delivery; real platform failures and separate Set-Cookie append unchanged | Unchanged source; finite carrier already supplies exact bytes |
| rust/crates/gateway-worker/src/admin_support_host.rs :: fn catalogue_error, fn catalogue_lift_preserves_the_exact_original_gateway_error | GatewayError; test Result<(),Box<dyn Error>>; Pure diagnostic lift/test observation; no fetch or mutation transfer | Unchanged source |
| rust/crates/gateway-routing-runtime/src/session.rs :: fn from_local_admission, with_cleanup_diagnostic | RoutingResult<Self>; Pretransport planning after same original admission; original-error/fail_planning/one-cleanup order unchanged | Unchanged source; no Result port needed |

Worker error_response is not universally pretransport: its actual callers can follow attempts, selected-response preparation and cleanup. It retains Response::from_bytes, original status, JSON/no-store, original header overrides, then awaited capture at the same points. Local response_error remains after the selected QuotaReplacement choice, before wrapper/delivery. Its LocalLifecycle and wrapper-error bodies are byte-identical. runtime::auth_response remains unchanged, including actual construction failures and separate Set-Cookie append semantics. The routing from_local_admission caller retains the same original admission and original-error/fail_planning cleanup order.

The manifest DAG is acyclic by source observation. gateway-source defines the single encoder; gateway-local-refusal adds only selected serde; auth/extras/schema depend directly on the defining data owners; catalog/admin/Worker consume these data. No reverse Worker/auth/Authority dependency is added to the source/refusal foundation. Root Cargo, external lock records, versions/features/profiles, baseline and source primitive bodies are unchanged. The separate mechanical-imports-and-cargo.patch excludes operation-body changes and is bound to this candidate's base, not to newer foreign branches.

## Unlisted owner ports and stale observations

No owner-controlled newer branch was fetched, merged or copied. The following source remains unchanged except the explicitly mapped MIME observation in the existing local-lifecycle file. Root/owners must relocate by actual function and input hash onto their current physical paths; these candidate paths are not permission to overwrite newer files. A data carrier checkpoint is not a claim that those exhaustive consumers compile.

- Claude-usage/A2: rust/crates/gateway-claude-usage-admin/src/readback.rs; Result<SourceJson,GatewayError>; admin readback DATA projection. Exhaustive AuthResponseBody match needs finite ClosedError coverage. Existing SourceJson missing claude_code branch returns checked null; no new null/failure arm or collector behavior supplied by D.
- Worker Extras: rust/crates/gateway-worker/src/extras.rs; Result<Response,GatewayError>; parse rejection before admission, render after runtime/child completion before delivery. Use actual ExtrasResponseBody tuple with existing as_bytes, and closed_error at the same parse-rejection point. FinishHook/settle_child/dispatch/refund/accounting are unchanged and unleased.
- Migration tests: rust/crates/gateway-migration/tests/profile_runtime.rs; Value-returning test-only scalar observation. Exhaustive AuthResponseBody match needs explicit finite-body test observation preserving all scalar fixtures/assertions; D invents no panic/default/null arm.
- Extras runtime tests: rust/crates/gateway-extras-runtime/tests/runtime.rs; Value-returning test-only scalar observation. SourceJson helper parameter does not accept ExtrasResponseBody; owner must select explicit checked byte observation, retaining all cases/assertions.
- Extras runtime tests: rust/crates/gateway-extras-runtime/tests/decisions.rs; Result-capable test-only response observation. SourceJson-specific body read needs explicit checked finite-carrier bytes. Decisions/image/dispatch semantics remain held.
- Worker/routing constructor integration: rust/crates/gateway-worker/src/inference_local_lifecycle.rs; RoutingError-returning physical wrapper-failure path. Pre-existing literal cannot construct private routing kind. Same-data ordinary RoutingError::new port remains outside this MIME-only cut; preserve plain500/status/headers/false tag/evaluation order.
- Schema tests/controller: rust/crates/gateway-schema/tests/source_request.rs; Test-only scalar error message/param observations. Pre-existing body-specific deserialize reads need typed or explicit checked byte observations. No generic deserialize facade added to ClosedErrorBody.
- Routing tests/controller: rust/crates/gateway-routing-runtime/tests/source_plan.rs; Test-only source validation observation. Locally constructed errors are ClosedError; retained arbitrary upstream Json remains valid. Preserve message/param assertions through explicit finite observations.
- Routing tests/controller: rust/crates/gateway-routing-runtime/tests/source_factory.rs; Test-only pretransport original-admission cleanup assertions. Finite-body observation port needed; preserve reservation/commit/pool assertions and original one-cleanup order.
- Routing tests/controller: rust/crates/gateway-routing-runtime/tests/source_contracts.rs; Test-only source-plan error observation before captured-selection work. Finite message/param port needed; retained target is already oversized and full structural extension is separately held.
- Routing tests/controller: rust/crates/gateway-routing-runtime/tests/staged_claude.rs; Test-only staged preparation rejection. Explicit ClosedError observation needed for local source error; sealed Claude phase/body and admission/settlement assertions remain exact.

The pre-existing removed Core/auth import spellings elsewhere also require the canonical mechanical defining-import integration. No facade or future six-owner Core API was guessed. Known wrapper-error and test representation failures remain explicit, rather than hidden by a blanket conversion policy.

## Cases and physical ownership

All 109 original provider names, fifteen prior regressions and five immutable fixture files remain byte-identical to the phase input. Existing local-refusal seven-case and nine-case targets are untouched. Eleven new cases are authored, not executed:

- rust/crates/gateway-auth-contracts/tests/closed_error_body.rs :: closed_auth_response_keeps_defaults_utf16_and_original_status — NOT RUN.
- rust/crates/gateway-auth-contracts/tests/closed_error_body.rs :: closed_auth_generic_json_and_direct_wire_views_match — NOT RUN.
- rust/crates/gateway-auth-contracts/tests/closed_error_body.rs :: existing_auth_source_json_and_empty_body_contracts_stay_distinct — NOT RUN.
- rust/crates/gateway-extras-contract/tests/closed_error_body.rs :: closed_extras_response_keeps_defaults_and_original_utf16_bytes — NOT RUN.
- rust/crates/gateway-extras-contract/tests/closed_error_body.rs :: extras_json_and_closed_wire_equality_keeps_original_source_identity — NOT RUN.
- rust/crates/gateway-extras-contract/tests/closed_error_body.rs :: extras_generic_serializer_retains_its_actual_failure_boundary — NOT RUN.
- rust/crates/gateway-local-refusal/tests/closed_error_serialize.rs :: finite_generic_serialization_matches_wire_without_serializing_the_private_kind — NOT RUN.
- rust/crates/gateway-local-refusal/tests/closed_error_serialize.rs :: finite_generic_serializer_preserves_actual_writer_failure — NOT RUN.
- rust/crates/gateway-schema/tests/closed_reasoning_message.rs :: reasoning_error_message_and_parent_param_stay_at_the_original_validation_point — NOT RUN.
- rust/crates/gateway-schema/tests/closed_reasoning_message.rs :: nonempty_lone_unit_reasoning_remains_open_source_data — NOT RUN.
- rust/crates/gateway-admin-runtime/tests/errors_utf16.rs :: closed_validation_errors_keep_original_bytes_before_defaults_or_selection_effects — NOT RUN.

The carrier vectors cover exact lone/paired UTF16, NUL/quotes, untrimmed type, absent/null/text params, original status/bytes/MIME/no-store, independent Set-Cookie DATA and header overrides, arbitrary duplicate/source JSON and Empty distinctions, private-tag nonserialization, real serializer writer failure, reasoning parent params/open tiers, and validation rejection before store/query/mutation effects. They do not prove actual Worker physical delivery, confinement or completed accounting.

Physical counts include all src Rust files and inline tests, not merely root files. This is source inventory, not the required actual Rust AST gate. The twenty provider/P1/refusal owners remain below1000; the largest is provider-chat841. Changed/affected owners:

| Crate | Input | Current | Status |
| --- | ---: | ---: | --- |
| gateway-admin-runtime | 5483 | 5483 | incoming oversized debt; no growth |
| gateway-auth-contracts | 839 | 855 | bounded |
| gateway-catalog-provider | 946 | 944 | bounded |
| gateway-catalog-runtime | 3354 | 3344 | incoming oversized debt; no growth |
| gateway-extras-contract | 400 | 440 | bounded |
| gateway-local-refusal | 294 | 325 | bounded |
| gateway-schema | 7497 | 7494 | incoming oversized debt; no growth |
| gateway-worker | 18553 | 18540 | incoming oversized debt; no growth |

New standalone targets are local-refusal40, Auth66, Extras66 and schema42; the modified Admin target is362. All remain under1000. Incoming catalog-runtime targets admin1018/control1306/runtime1520 remain unchanged and need their owner structural work. Full Schema/Routing structural extensions remain separately held; actual Routing production count is 7867. No ceiling or allowance is edited to pass this cut.

One pinned rustfmt mutation pass covered only seventeen exact changed Rust files with repository config and skip_children=true; exact-file --check and git diff --check returned0. The source-only mechanical graph/physical/hash observations are recorded with full outputs. No compiler, Clippy, tests, parser gate, build, Worker, provider, service, credential, private-state or CI operation ran. All current affected compiler/native/WASM/comparator/golden/runtime/Mac/client gates remain pending; historical green receipts are not promoted.

## Explicitly held policies and next order

- ReplayStore preparation versus postterminal remember; canonical bytes/depth64/source parity and actual publication/failure disposition remain held.
- ProviderEvent<Value> scalar construction versus validated SourceJson source constructors and failure_event phase remain held.
- resolve_paid_route closed billing and actual Authority/routing same-phase callers remain held.
- Image plans/extract source carriers and their actual child-dispatch/accounting consumers remain held.
- The total forwarded_payload_policy declaration and actual native_catalog rendering/casing remain held.
- Typed local aggregate-overflow termination stays preserved; native created+exactly-one-failed physical stream mapping/client/workerd acceptance remain held.
- Non-JSON raw refusal bytes/status/headers retain their legacy raw_body plus internal JSON-null sentinel representation; no absence/raw redesign here.

First preserve this exact source/API locally. Root then issues the narrow owner ports against current physical sources, composes the canonical defining-import closure, and runs the minimum selected native/WASM/strict-comparator/structural/test and real runtime acceptance. No completed inference may be retroactively failed/refunded or publish a fake receipt to solve an interface. Full provider/P1 and the whole Rust port remain unfinished. Source writes stop at PRESERVE_READY until the coordinator's local SHA receipt.
