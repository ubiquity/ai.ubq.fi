# AC source family contract v1

Controller: controller01a17ebe. Module: admin-claude-runtime-family, aid10 c4da650491.
Fixed source: 7694a49787c7379180cb0e268b8b03fac093d879, tree 7a950bf53c082af013bf02d9deb12d5819c14723.
Requested model/effort: gpt-6.1-sol/max. Actual model/effort: unknown/unknown; no trusted active hook was exposed.
Compiler authority: none. No alternate runtime, target, environment, child agent or fallback was provisioned.

Objective: partition the entire remaining Admin and Claude runtime family into conventional bounded defining owners,
preserving the original complete Rust port objective. This checkpoint is provisional source, not acceptance or completion
of the Rust port. Root owns canonical integration and subsequent execution. No branch/base switching, fetch, push, CI,
compiler, Clippy, test driver, gateway-quality parser, native build, Worker build, network, provider, credential,
private-state, service, browser, SSH or home/global configuration operation was performed.

Non-goals: behavior changes, credential validation/bypass, routing or compaction changes, quota/paid proof expansion,
live migration, historical telemetry classification changes, delivery acceptance or compile-performance claims.

## Invariants and behavior matrix

| Surface | Retained order and ownership |
| --- | --- |
| Admin | Existing actor admin check, clock range check, context, then dispatch/reads; no super-admin gate |
| Defaults/kernel | Existing row reads, cutover claims, reservation reconciliation, selector/collision rules and final CAS |
| Paid administration | Existing pricing inputs, revoked-key guard, outstanding ledger checks, per-row fences, final key hooks and cleanup |
| Claude roster | Credential source field/hint encoding, immutable redacted secret DATA, roster/cohort identity, tombstones, CAS and isolate generation |
| Claude inference | Principal access, current roster, catalog generation and native revisions; opaque selection; actual header time; one consumed head; final quota CAS |
| Workspace | Pure fixed response selection moves; checks, owner update and same-admission continuation remain execution-owned |
| Direct usage | Existing fixed GET collector, exact account/cohort lease, usage/usage_snapshot preservation, accepted observation and history/readback; no inference |
| Observation failures | Accepted quota/proof is not undone by a failed passive observation |
| Transport | Unknown transport failures and explicit refusals remain distinct; no fabricated exhaustion or paid permission |

## Owned source and dependency order

1. `gateway-admin-runtime-records`: named record data, bounded reads and Claude access policy.
2. `gateway-claude-roster`: complete credential/roster/generation owner, including the private secret representation.
3. `gateway-admin-runtime-contracts`: finite host request/response DTOs, source parsers and RuntimeConfig DATA.
4. `gateway-admin-kernel-runtime`: actual policy/window operations and reservation reconciliation.
5. `gateway-admin-settings-runtime`: defaults/debug/selection/principal settings; borrows the same kernel service for its existing operations.
6. `gateway-admin-paid-runtime`: paid pricing and the entire deletion-fence operation.
7. `gateway-admin-diagnostics-runtime`: error-history operation and source brief framing/projection.
8. Retained `gateway-admin-runtime`: authentication/clock-before-dispatch composition and existing AdminHost hooks.
9. `gateway-claude-policy`: ordinary host DATA/finite traits, response classification, quota transition DATA, named direct-usage write and pure workspace projection.
10. Retained `gateway-claude-runtime`: SelectedAccount/AcceptedHead, sole minting paths, select/prepare/ensure/observe/finalize, workspace effects and final inference quota CAS.

Existing `gateway-admin-test-support` and `gateway-claude-runtime-test-support` remain dev-only support responsibilities.
The new public source-record target moves seven genuine public-behavior cases; the one case using private roster_source/empty
remains private. Shared original fixture helpers move to the existing bounded admin test-support owner. Secret visibility
is unchanged. Production packages do not gain normal edges to test support.

`direct-import-moves.json` enumerates the exact owned/direct-caller source paths and defining-path moves. Caller bodies
outside this family receive nominal imports/paths only. `registrations.json` lists eight new owners and the exact seventeen
first-party manifest/lock records. External lock package bytes, versions/checksums/features, workspace profiles, baseline
and all unrelated source are preserved. No C/D, Auth, Authority, Native, routing, migration, transport or compaction operation
body is owned here. Worker and migration files are mechanical direct callers only.

## API and phase map

`defining-api.json` inventories declarations. `family-dag.json` records normal edges and the actual acyclic closure.
`SelectedAccount` fields remain crate-private in execution; prepare remains crate-private. AcceptedHead has no Clone or
Deserialize and all fields remain crate-private. No new constructor accepts caller permission/proof, arbitrary Store/Tx,
callback or JSON authority. Existing StateSnapshot owner/number and ClaudeSourceSecret UTF16 remain private.

The previously crate-private Outcome/merge/clear are moved as finite quota transition DATA, not opaque authority.
They retain their exact fields, arithmetic and source body. PoolProof was and remains a public serializable record
in the pre-existing usage-quota DATA owner; it is not a private authorization capability. Only the unchanged execution
persist operation performs the inference quota CAS after current-binding checks. The existing public record_usage
operation stays with its merge/observation definition and keeps both current roster and quota checks in its write.

The dispatcher substitutes finite borrowed services containing exactly its existing store, host, capabilities and (for
settings) RuntimeConfig references. Defaults' three existing kernel operation receivers use that same borrowed service.
There is no stored service, new lifetime, adapter, policy fallback or additional read/CAS. Two original inherent cleanup
method names in key_deletion tests now name the already-existing AdminHost finish_key_deletion/abort_key_deletion hooks;
those hooks call the same moved operation. Original case names, assertions, fixtures and other bodies remain retained.

Current Cbc8 source imports use gateway-source, gateway-auth-contracts and gateway-auth-primitives defining APIs.
`held-core-nominal-ports.json` binds the fixed map SHA 843c705afd5441843191ad312c28c809df0df8a2d24ff21c460bc815fffca438
and enumerates current per-file nominal Core imports for Root to port **after** canonical Core integration. No 8c5 bodies,
future types, foreign merge, compatibility facade, copied encoder or guessed C/D adapter is adopted. Old partial Core APIs
on 7694 remain intentional. Existing foreign legacy imports are not evidence of an adopted future Core and are not repaired
by rewriting unowned semantics.

## Acceptance and execution boundary

Selected source paths are those in formatter-paths.json and registrations.json. Bounded allowed checks: static source/body,
manifest, DAG, test-name, fixture/hash and physical conventional module-closure inventories; pinned scoped rustfmt; git diff --check.
These inventories are not AST/type/privacy proofs. Rustfmt mutation pass 1 reported a struct-literal receiver syntax error;
pass 2 succeeded. The final read-only --check succeeded. Exactly two formatter mutation passes were used.

Strict native all-target and selected WASM Clippy with unchanged staged comparison, repository structural parser,
original confined named Admin/Claude/usage tests, real SQLite race/reopen tests, selected shipping Worker artifact and all
matching-platform/provider/auth/UI/client acceptance remain NOT EXECUTED. Root retains their lease and final acceptance.
The quality baseline was not updated; Root must perform any authorized tightening after real structural verification.

Unknowns: type/compiler/staged lint behavior on this exact partition; final canonical Core nominal paths after supplied
adoption; external platform/runtime acceptance. No OS confinement, compiled artifact, runtime success, live identity,
private-state authority or historical recovery receipt is claimed.

Stop condition: coherent full-family source partition, all owned physical source/target closures <=1000, no oversized
foreign growth, provenance and held interfaces recorded, genuine provisional commit and PRESERVE_READY. The source is
then frozen until coordinator export/readback LOCAL_PRESERVED; this worker neither self-polls nor begins another phase.
