# Recovered concrete auth DATA/API v1

This is the continuing full auth-family contract v7, based on approved NEW recovery commit
34925e53677f95d4cab891a426765e04d2042cd0/tree 62bfc7373db84f212b72455ec2b1b2be938e2148,
with b592 as the behavior oracle. It is not a reconstruction claim for historical missing commit identities.
The exact new frozen source commit is the accompanying PRESERVE_READY manifest.head/source.bundle HEAD.
The old a976 foundation remains unchanged. `recovery-api-v1.json` supplies exact defining source, hashes,
all current production references and owned physical source inventory; qualification receipts are separate.

## Concrete checked operation and producer chain

Defining path: `gateway_auth::resolve::AuthService`, with the inherent operation in `passport_current.rs`.

```rust
impl<S: AuthorityStore, H: AuthHost + PassportDate> AuthService<'_, S, H> {
    pub async fn check_passport_app_session(
        &self, binding: &PassportAppSessionBinding,
    ) -> Result<PassportAppSessionData, GatewayError>;
}
```

`AuthService::new(store: &'a S, host: &'a H, config: &'a AuthConfig) -> Self` holds the real store/host.
AuthHost extends WebAuthnCrypto; PassportDate is a separate defining finite host primitive. The service
reads the exact local row and actual stored APP token through PassportSessions::check_bound, performs the
fixed central introspection outside an Authority transaction, validates the complete source response,
then rereads and compares the entire row binding. It accepts no caller verification callback or token
reconstructed from actor DATA. It returns DATA, never an authority proof.

Authenticated browser flow is AuthService::authenticate_client/admin -> passport_cookie ->
PassportSessions::authenticate -> PassportSessionData::App(PassportAppSessionData { identity,binding }).
The public method kind stays passport_session. Browser route dispatch calls PassportBrowser; legacy
JWT/bearer verification remains strict and separate.

## Exact defining DATA and source domains

| Defining type / operation | Exact contract |
| --- | --- |
| PassportAppSessionBinding in gateway-auth-passport-contracts::actor_data | session_digest, row_revision, subject, created_at, app_token_digest, client_id, audience, origin, redirect_uri. Digests are 64 lowercase hex. Subject is 1..500 UTF16 units; createdAt 1..100 exact original units. Fixed ai/ai/https://ai.ubq.fi/https://ai.ubq.fi/auth/callback. row_revision is the exact store revision, not a fabricated UUID/time or standalone permission. |
| PassportSessionIdentity in session | subject/handle/scopes use JsString16; is_admin bool. Central handle0..500, <=128 scopes each0..500; exact subject/user id/date/binding equality required. No cached role/scope fallback. |
| PassportAppSessionData | identity plus binding only, returned after the concrete check. It is cloneable DATA, not the authority owner's non-Clone single-use effect value. No APP/local token in it. |
| PassportActorDataV2 | Explicit envelope version2, kind passport_app_session or passport_jwt, data. App binds exact local digest/revision/subject/createdAt/APP-token digest/fixed binding, no role/scope/expiry. Codec validates DATA shape only. |
| PassportJwtBindingV2 | Lossless sub/jti1..500 units and mandatory expires_at_ms:i64. No fake jti, optional expiry or persistent JWT. Explicit new source JSON/framed domain; not historical native-v1 framing or a MAC proof. |
| PassportSourceClaims | Mandatory safe-integer iat/exp seconds; exp>now and>iat, iat<=now+60. Source sub/jti1..500, auth1..160; handle missing -> empty, present0..500; scopes missing ->empty, <=128 strings each0..500; agentTokenId absent/null or1..500. Actual signature verification precedes authentication. |
| PassportDate::parse_date_ms(&JsString16)->f64 | Finite actual JS Date.parse plus original string equality, not ISO-only parsing/normalization/time-derived expiry. PlatformHost passes JS UTF16 directly to Date.parse via Reflect. Engine unavailability returns NaN and fails that path; no accepting fallback. |
| PrincipalId | from_scalar(String) keeps the old infallible scalar constructor domain. from_source(JsString16), as_source, try_scalar, explicit source codecs preserve exact units. Other opaque IDs/guards remain unchanged. Legacy default serde stays scalar-only. |
| JsString16::json_token(&self)->String | D's only new defining primitive seam. One existing UTF16 token encoder, now public and assertion-free through decode_utf16 plus exact lowercase escapes. It returns text, not SourceJson or authority; generic serde/raw parsing stay fallible. D owns ClosedErrorBody and all its error policy. |

A local session token has exact prefix uos_ai_passport_session_ plus43 URL-safe ASCII characters from32 random bytes.
The key is the existing tuple codec for [uos_ai,auth,passport,sessions,SHA256(local token)]. The record is logical
version1 with subject/fixed binding/createdAt/appSessionToken and expires_at_ms=None. None means **no server TTL**;
it does not mean an infinite JWT, cached role, currentness or permission. Browser400-day persistence is renewed
only after successful auth report. APP tokens have their separate fixed prefix and43-character suffix.

Creation validates the grant/subject/date/token/binding before any change, then absent-new-key CAS and optional
previous local deletion occur in one atomic transition. Old grant revoke is best effort only after commit.
Each use POSTs fixed /api/auth/sso/session with the stored APP bearer. Missing local token/row is401; malformed
record/store/network/non401/malformed reply is503 preserving state. Authoritative401 deletes local authority
even when Worker rejected-body cancellation fails. Successful introspection rereads the exact binding before
return; logout/replacement wins, with no republish. Logout deletes locally before independent best-effort revoke.

The passkey revision index stays its separate uos_ai:auth:session_revision_index:v1 domain with source token
digest, tuple identity, revisions and expiry metadata. Read-current DATA never grants authority. Persistent
Passport rows do not acquire passkey TTL or replace that index.

## Legacy JWT and new lossless provenance

AuthMethod is defined in gateway-auth-contracts::types: PassportSession { identity:PassportSessionData }
and PassportBearer { claims:PassportSourceClaims }; principal_id returns Core PrincipalId.
PassportSessionData distinguishes Jwt(source claims) and App(identity+binding). Its private legacy serde
adapter retains the old scalar kind/claims bytes via unchanged PassportClaims, mandatory exp and jti.
App and non-scalar source identities refuse that historical scalar codec. The explicit v2 JWT DATA handles
source-valid lone-unit/NUL sub/jti; real RSA auth/report and lossless schema cases are in the fresh component
test logs. Actual native v2 actor mint/MAC/capture/currentness integration is still pending its owners.

SourceActor/authority semantics remain byte-identical to M67 (whole gateway-authority tree
c07a5c60ea6b91c1a45f80ae5a8aeb28eee95ba4). In particular the existing actor.rs:339/674 claims/String
consumers need authority_continue's adaptation. DATA decoding never reconstructs a live proof.

## Semantic consumer/ownership map

| Owner / exact seam | Required integration; no implicit body-write lease |
| --- | --- |
| authority_continue: gateway-authority/src/native_control/actor.rs, SourceActor mint/read/currentness and effects | Consume App.binding or explicit lossless JWT DATA. Hold the concrete AuthService operation; mint the owner's private single-use non-Clone/non-Deserialize checked value. Bind exact local row revision/token/subject/binding into the effect's final transaction/CAS. Recheck freshly after every effect/native await; refuse v2 on store-only paths without authenticated preflight. No remote I/O inside transactions or serialized authority bool. |
| native_continue: native receiver/fence/control/action/currentness | Consume only after authority's frozen semantic API. Preserve v1 bytes/selectors/limits/captures; explicitly bind new v2 lossless version in MAC/digest and actual before/after socket checks. No public proof ctor or arbitrary-signature callback. |
| C named DATA: Core PrincipalId/RequestContext; gateway-admin/src/audit.rs::ChangeActor; live mapping/protocol principal fields | Exact source units through principal -> nullable audit/live DATA and existing Json/ProfileV2/tuple codecs. Account/call/request IDs retain their own scalar guards. Existing comparisons use source units. No authority or live-operation policy transferred. |
| C named Worker auth: auth_passport_host.rs, AuthHost/WebAuthnCrypto implementations and auth-only runtime seams | Original physical request/cancellation/store/native binding, fixed finite auth operations, actual clock/RNG/WebCrypto/Date.parse. Ordinary CRL/JWKS behavior does not inherit new global caps. Append each Set-Cookie separately. Compiled common-Authority runtime cases remain pending. |
| D local-refusal ClosedErrorBody | Consume only the public existing json_token defining operation. D freezes its producer/consumer API; no unchecked RawValue constructor or SourceJson redesign is granted. |
| ROOT-assigned migration/inverse/custody | Validate/preserve the separate no-TTL Passport family, raw keys/revisions/units and versions. Actor/fence DATA imports cannot mint proof. Current broad auth selector is not proof of typed ProfileV2 schema/migration acceptance. |

## Qualification and remaining gates

Qualification applies to the explicit mechanical validation view, with full source postimage hashes and
command/stdout/stderr receipts under handoff-evidence/recovery-20261027T143350Z. Feature source and external
registration/import/lock patch are returned separately. The bare feature root lock is deliberately not
regenerated; apply the named mechanical patch to qualify the same package source in an integrated workspace.
Native here means actual GNU x86_64 portable components, not Mac/native-custody acceptance.

Final fresh case counts, parser closures and Clippy/WASM outcomes are in recovery-qualification-v1.json.
Old190/194-test receipts are historical only. Missing actual Worker tools/typed JS qualification, central
Date.parse comparison, actor/effect/native/capture/migration integration, real Mac and browser/client acceptance
remain explicit gates. No source branch push, credential/capture adoption or deployment occurs.

D dependency metadata readback is4389 bytes/SHA2566f1d31a877b4cd4ad51929f7c8fca368de1afe013a54b3e3239d19836732ee79.
The selected defining API is `gateway_source::js_string::JsString16::json_token(&self) -> String`.
D currently uses its old Core string type; ROOT retains the lease for integrating C's defining foundation
and direct defining consumer imports. C changed no D/Core source, copied no encoder and added no type
conversion facade/backedge.
