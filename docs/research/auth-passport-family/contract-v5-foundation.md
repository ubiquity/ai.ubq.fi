# Contract v5 — root-leased Passport DATA/API foundation

Decision file `passport-foundation-decision-v2.md` verified at exactly 7051 bytes and SHA256
1ecd8b2173a0b47e2dc4d5011393577241f59b01a788e3a691f92ca49a1a4a44. Full original auth objective continues.
`foundation-v2-inventory.json` freezes pre-edit paths/hashes, physical counts and candidate consumer inventory.
This foundation is a separately committed API source dependency before C's dependent auth integration and before
rust_authority_size / rust_migration semantic implementation. Affected scalar v2 evidence is stale; M64 is unchanged.

## Owned design and cycle resolution

- Define gateway-source from existing core error.rs, js_string.rs, js_string_case.rs, source_json.rs and
  source_serialization.rs. All error fields/codes/status/omission/formatting behavior stay exact; moving unchanged
  GatewayError with the primitives removes the otherwise real Core error backedge. No error/store redesign.
- Remove SourceJson's PortableValue constructor and recursive writer from that leaf. Implement the same conversion
  in PortableValue::to_source_json at its defining core value owner. Callers move mechanically to the actual owner;
  no re-export, path facade, cfg hiding or replacement codec. Existing raw validation/UTF16 tests remain intact.
- Core PrincipalId gets explicit scalar and source construction, exact-unit equality and lossless source JSON access.
  The old Core tuple constructor was infallible: this is recorded rather than inventing UUID/trim guards. Preserve
  scalar construction/legacy serde and the existing caller-owned scalar guards; source construction is separate.
  Other opaque_id! domains and finite call/action/account/native selectors remain unchanged.
- Auth Passport DATA defines a distinct JWT-versus-app identity, v2 app source binding and separately versioned
  lossless JWT actor DATA proposal with mandatory jti/expiry. It must not weaken historical native v1 bytes.
- Concrete constructor-held Passport service check takes only the finite binding DATA, reads the actual authoritative
  row/app token, performs fixed central introspection outside Authority transactions, validates all source guards,
  then rereads exact revision/token/subject/binding. Return DATA, never a deserializable permission or stored role grant.
  Authority's own private single-use currentness factory must call it; C does not implement authority semantics.
- Named nullable audit/live identity DATA use source carriers and the existing explicit Json/ProfileV2 record codec.
  The scalar constructors/legacy record interpretations remain available; no account/call/request ID broadening.
  Actual actor/native currentness, custody interpretation and migration semantic adapters remain assigned owners.

## Exact first edit surface and consumers

Initial core symbols: lib module declarations; GatewayError/OpenAiError/OpenAiErrorEnvelope defining module move;
JsString16 and units/optional_units, lowercase_source, SourceJson and source serialization move; PortableValue's
source JSON conversion; PrincipalId only in identity.rs. New gateway-source package, its conventional modules and
bounded tests. Auth-owned session/contracts/checked-operation modules are listed in the inventory. Audit defining
DATA is admin tip gateway-admin-contracts::audit::{ChangeActor,event_mutation,wire_record,decode_event}; live DATA is
mapping::{Mapping,write_mapping,read_mapping}. Scope is DATA/codec evolution, not administrative or live authorization.
All external imports/manifests/lock/registration and conversion call repairs are candidate-only mechanical commits
with per-file input/output hashes and defining edges; other owners' function bodies remain held.

Source-only fixed review, parser caps and exact source-preserving positive/negative fixtures precede the separate
foundation freeze. Fresh compiler/lint/tests still require the C filesystem lease plus kernel lock and unique overlay
targets. No phase or tool gap lowers acceptance. After freeze publish exact API, commit/tree and consumer list to root,
then continue owned auth integration while authority/native writers consume that same frozen source.
