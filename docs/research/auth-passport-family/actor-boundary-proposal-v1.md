# Frozen proposal v1: Passport app-session actor and lossless identity seams

For root adjudication and `rust_authority_size`; discovery/proposal only, **no external semantic writes authorized**.
Source is b59243d674567d418d1410f8390fc7fe47a08c92 (tree 647c8a42378d85fa747cb6ac3de07c822798953f),
with exact M67/admin/M60 defining moves. This does not change the sealed auth contract or legacy JWT SourceActor.

## Exact accepted domains (all string lengths below are JavaScript UTF16 units)

| Field / provenance | Exact b592 guard and producer | Required carrier / restriction |
| --- | --- | --- |
| Passport subject / JWT `sub` | passport.ts:157-181: string, length 1..500. No trim, UUID, ASCII, normalization or scalar guard. Sessions grant.subject equals verified sub; record.subject same bound; introspection session.subject and user.id equal that exact subject (sessions.ts:43-52,77-109,124-143). | JsString16 for the app identity and its provenance, including lone units, NUL and whitespace. Do not infer a UUID or widen other IDs. |
| Central `user.id` | Required exact equality with subject; equality alone is insufficient without a valid bound local record. | Same exact source units, not a separately normalized identifier. |
| Central `user.handle` | Required string, length 0..500; empty allowed, no normalization. Legacy JWT handle may be absent (reported as empty); explicit null is refused. | JsString16; audit/display must preserve it without 200-unit clipping. |
| Central scopes | Required array <=128, each a string length 0..500. Legacy JWT scopes may be absent (reported as []); explicit null refused. | Vec<JsString16>; no role from transport success or stored scope snapshot. |
| `isAdmin` | Required boolean, both JWT and central user. Persistent admin uses current central result; no super-admin. | Current auth DATA only, never a serialized proof/authority boolean. |
| JWT `jti`, `auth`, `agentTokenId` | jti string 1..500; auth string 1..160; agentTokenId absent/null or string 1..500. These guards do not impose ASCII/scalar domains. Callback rejects every non-null/non-absent agentTokenId after real JWT verification. | Keep legacy SourceActor::Passport { sub:String,jti:String,expires_at_ms:i64 } and its behavior unchanged. App actors have no jti/auth-token stand-ins. This proposal does not silently repair or broaden pre-existing scalar legacy actor behavior. |
| JWT `iat` / `exp` | Required safe integer numbers; iat <= floor(now/1000)+60, exp > now and > iat. | Mandatory strict JWT expiry remains mandatory; no Option conversion. |
| `iss`, `clientId`, audience, origin, redirectUri | Exact constants https://passport.ubq.fi, ai, ai, https://ai.ubq.fi, https://ai.ubq.fi/auth/callback. No forwarded-host binding. | Fixed scalar constants, not general UTF16 IDs or caller values. |
| `createdAt` | Required string length 1..100 with finite actual Date.parse. Preserve original string; introspection must equal it exactly. Non-ISO parseable strings are accepted. | JsString16 plus actual JS Date.parse before authority staging, never normalized ISO, scalar-only parser or expiry. |
| Browser local token | /^uos_ai_passport_session_[A-Za-z0-9_-]{43}$/; 32 fresh random bytes. | ASCII secret; only SHA256(local token) appears in key/provenance. |
| Central app token | /^uos_app_session_[A-Za-z0-9_-]{43}$/; grant.token becomes record.appSessionToken. | ASCII secret retained only in authoritative session row and fixed upstream Bearer; never actor/fence/audit/wire/capture credential material. |
| Session digest / proposed app-token digest | Exactly lowercase 64-hex SHA256 bytes; local digest already names source key. App-token digest is proposed opaque DATA for exact token binding, not authority. | String/validated digest; no new loose ID domain. |
| Store revision | Actual opaque AuthorityStore revision; exact equality/CAS, no caller string minted as currentness. | Preserve store revision and absence/presence. Do not add trim, UUID, safe-number or date assumptions. |
| Public principal | passport.ts:214,234 + handler/http.ts:56-66: exact `passport-user:` + subject. No subsequent trim/normalization. | Lossless principal carrier; prefix concatenates units. No escape-text/hash/scalar substitute. |
| Audit actor ID / label | native-api-key-audit.ts:88-111: Passport sub/handle copied directly, kind remains passkey_user; not public principal prefix. No additional guard/truncation. Shared credentials retain null/null. | Only these nullable fields become lossless; kind/super-admin/target key IDs/request IDs remain their existing contracts. |
| Live mapping principal_id | live/upstream.ts:139-165 writes the resolved principal unchanged; read uses utils.ts:3 getString (typeof string only, including empty), legacy absent/non-string -> null. New authenticated Passport principal is nonempty by construction. | Lossless optional principal, retain legacy null fail-closed behavior and mapping retirement/expiry checks. |
| Other IDs | RequestId, ApiKeyId, account IDs, live call IDs, owner/epoch/action/connection references are not Passport subjects. In particular private live request references use native_live/wire.rs::reference: ASCII alnum or -_:. and length 1..256; live Location accepts rtc_ with nonempty suffix or canonical UUID (live/upstream.ts:100-116). | No blanket replacement of core opaque_id! macro or broadening these source/finite protocol guards. Only PrincipalId and proven identity/display fields are proposed below. |

## Session authority and proposed API

Authoritative source key is exactly `[uos_ai,auth,passport,sessions,SHA256(local token)]` using the existing tuple codec.
Record is logical version 1, subject, four fixed binding fields, createdAt, appSessionToken; **expiry=None means no server TTL**,
not a current-role grant, infinity, a lease, a passkey expiry or browser lifetime. Browser Max-Age is separately 34,560,000s.
Use the existing Json/ProfileV2 finite SourceJson codec, preserving raw revision, absence and absolute expiry metadata.
Do not repurpose passkey `[uos_ai,auth,session_revision_index,v1,digest]` or its domain-separated digest/expiry schema.
The Passport source row itself is its durable revocation index; any additional durable index would need explicit owner approval
and atomic source/index writes, rather than being guessed here.

```rust
// Proposed auth DATA shape; exact final names belong to the single leased foundation writer.
AuthMethod::PassportSession { identity: PassportSessionData }
enum PassportSessionData {
    Jwt(PassportClaims),                 // mandatory legacy exp/jti remain intact
    App(PassportSessionIdentity),         // current subject/handle/role/scopes; no JWT
}
// Existing public report stays kind=passport_session, principal=passport-user:<subject>.
// App report auth=sso; exp/jti/agent_token_id are null. No second public kind/alias.
// Existing PassportBearer { claims: PassportClaims } stays separate and admin-refused.
```

Actual AuthService authenticates the original cookie, performs fixed central POST introspection **outside** an Authority
transaction, checks all response fields, then rereads the local row and same app token. Returned identity DATA cannot mint
NativeRenewalActor; its existing private factory still calls real AuthService. Creation validates the complete grant first,
then one absent-new-key CAS + optional prior-row deletion; prior app revoke is best effort only after commit.
Local logout deletes first. Authoritative upstream 401 deletes local authority even if cancelling its body fails; malformed,
store/network/non401 failures are 503 and preserve recoverable state. In-flight introspection never republishes a row.

Propose a NEW private `SourceActor::PassportAppSession` under an explicit stored-actor v2 schema, with exact subject,
local session digest, source row revision, createdAt and app-token digest. No raw local/app token, cached admin/scopes,
serialized authority boolean, public proof constructor or proof serde. NativeRenewalActor remains opaque.
Legacy stored v1 + SourceActor::Passport mint/read/current-time behavior, required expiry and old digest bytes remain unchanged.
New actor `expiry=None` still requires authoritative row existence/revision/token/subject/binding and current central authority;
owner/config/runtime fences and operation purpose remain required. Logout/replacement wins via the exact row CAS on preparation
and on every effect recheck, including after native awaits. A time-only validate_current_time call is insufficient for this variant.

**Required authority decision:** existing actor::read_current(tx,...) is transaction-only. Fresh role/revocation cannot be
inferred from a no-expiry row or cached success. Before every privileged effect/native recheck, authority must perform the
concrete fixed introspection operation outside the transaction, then create its own private short-lived currentness value
and recheck source row revision/token/subject in the transaction. A store-only read_current must refuse this variant when
that authenticated boundary is absent. No arbitrary verification callback or remote I/O inside a transaction may substitute.
Root must lease this complete preflight/effect boundary; merely adding an enum arm does not establish it.

## Complete seam / ownership map for the root lease

| Owner to assign | Exact defining/read/write symbols and consequences |
| --- | --- |
| C auth-private | gateway-auth-contracts::types::{AuthMethod,AuthPrincipal}, Passport identity DATA defining leaf, gateway-auth::resolve::{authenticate_client,authenticate_admin,method_wire}, passport browser/session services. Wait for frozen shared carrier/API commit before dependent integration. |
| ONE root-leased shared foundation writer | gateway-core/src/identity.rs::PrincipalId and RequestContext principal only, with existing JsString16/SourceJson support; leave other opaque IDs unchanged. Native source keys still use pinned TextEncoder conversion only at the shared tuple codec boundary (tuple_key::KeyPart::source_string_utf16), never replacement in logical identity. Enumerate all compiler-reachable principal conversions; conversion to scalar is not a general fallback. |
| rust_authority_size (semantic actor owner) | native_control/actor.rs::{SourceActor,StoredNativeActor,CurrentNativeActor,mint,read_current,ensure_lifetime,validate_current_time,source_keys,bind_preparation,principal_id,read_current_after_import_publication}; rows.rs::ActionRow, facts.rs::{session_context,read_context,validate_session_api}, live_renewal.rs::Control and mapped principal comparisons. Add source row to observed/currentness keys and effect-time rechecks, not just creation. |
| Same exact foundation/authority lease, coordinated native owner | native_control.rs::NativeRenewalFence principal, native_live/wire.rs::NativeLiveFence principal and actual response codec, native_live currentness/mapping readers, gateway-native/src/live_bridge/authority.rs fence equality/recheck. Preserve old v1 framing/MAC bytes and selectors. New lossless private response/schema must have explicit version handling; do not run UTF16 through serde Value/internally tagged Content buffers or assume M67 DATA owns authentication. Native socket.rs checks before dial, after dial and before send must continue through actual authority rechecks. |
| Root-leased audit writer (admin defining consumer) | b592 gateway-admin/src/audit.rs::{ChangeActor,actor,event_mutation,decode_event,wire_record}; exact admin tip defines these in gateway-admin-contracts/src/audit.rs. Only principal_id/principal_label need lossless nullable data; existing Json/ProfileV2 event writer already supports lossless names and must extend actor leaves, preserving 90-day TTL, timestamp selection, IDs, redaction and same lifecycle atomic commit. |
| Root-leased live writer | gateway-live-runtime/src/protocol.rs::principal_id; mapping.rs::{Mapping,write_mapping,read_mapping}, runtime.rs principal arguments/equality, gateway-worker/src/live_host.rs original RequestContext and actor comparison, lifecycle.rs::principal_id, runtime.rs admin context seam. New principal keeps equality, twice-read mapping/retirement and current-account checks; account/call/owner IDs keep their independent guards. |
| Migration/capture owner | native_control::custody::validate_native_control_custody (reached by migration inverse.rs:277/native_latest.rs:185), source record Json/ProfileV2 schema selection, core_state::decide and projection::validate_record for named Passport rows. Store v2 actor DATA/captures retain versions/revisions/expiry and lossless units; deserialization/capture never reconstructs a live actor/currentness proof. Imported no-expiry Passport rows require fixed key/binding/token/date validation and a fresh central request before use. Do not infer or adopt a live grant from imported state. |

JSON scalar-compatible historical rows remain ordinary Json; lone source units use existing ProfileV2, not literal escapes as
identities, new graph codecs, generic store decoding or proof constructors. Private v2 digests must bind their exact versioned
lossless representation; v1 digest/hash baselines are not transferred. Tests must include lone-unit subject/handle, empty handle,
500/501-unit boundaries, strict ASCII tokens/reference negatives, legacy JWT actor regressions, 503 preservation, 401 cancellation,
logout/replacement during introspection and after mint/before native effect, capture round trips and refusal without fresh preflight.
