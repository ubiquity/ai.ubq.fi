# auth-passport-family contract v2

Identity: P `/Users/nv/repos/ubiquity/HANDOFF-2026-10-18-ai-rust-wasm.md`, aid10 `d7452359b8`.
Source: exact M67 09cbf8b23dd3d21b56eb2ff9b27cd65bac468f65, tree 423d8388368045a9eb5489c514d5b147a07b82c1,
on public b59243d674567d418d1410f8390fc7fe47a08c92, tree 647c8a42378d85fa747cb6ac3de07c822798953f.
Admin 93f6ed2403468fc252ba3fdb9c8efb05fd285d8b and M60 cb24be5e1ee79f53c23ae0e687956ba7860acabb are
separate unaccepted defining-consumer inputs. Their ancestry will be merged only in the validation composition.
Model and effort: unknown (assignment is not runtime metadata). Host Linux x86_64, kernel 6.18.55.

## Objective and source evidence

Complete the remaining auth family under the physical 1000-line production and transitive test-target caps,
reuse unchanged M67 public native DATA, and implement b592 TypeScript persistent Passport sessions.
`src/auth/passport-sessions.ts:1-192`, `src/auth/passport.ts:1-368`, both corresponding TypeScript tests,
and the exact Rust source/test inventory are the design inputs. Machine inventory includes input hashes and symbols.
No deployment, live Passport/provider/native operations, credentials, private captures, source publication, or other owner body changes.

## Responsibility, producer, consumer and dependency matrix

| Defining owner | Source responsibilities / producers | Consumers / dependency edges |
| --- | --- | --- |
| gateway-auth-native-wire (reused) | M67 native_read, control, publication_command, publication_data, validation; strict finite DATA | native contracts, framing, proof owner, native/authority/Worker external callers |
| gateway-auth-primitives | Secret, crypto/hash, record key/decode/CAS, API-key records/policy, basic errors | all auth leaves; core, serde, JSON, base64, sha2 only |
| gateway-auth-passport-contracts | fixed binding/configuration and strict JWT claims, persistent session identity DATA | general contracts, Passport verification/browser/persistence; primitives, core, serde, URL |
| gateway-auth-contracts | AuthRequest/Response/Config/Method/Host, passkey session/ceremony DATA, WebAuthn crypto/signature DATA, finite upstream request/response | orchestration, passkey/attestation/Passport, Worker and fixtures; primitives, Passport contracts, native wire |
| gateway-auth-credentials | provider_credentials, native_credentials, managed_access together (759 lines before module declarations) | native authenticated publication, native owner/routing/external callers; primitives/core/serde/time |
| gateway-auth-native-contracts | import metadata/target and shared authority state/completion/catalog DATA, live framing, nonce host contract | import-authority DATA, framing, native proof owner; native wire, primitives/core/serde only |
| gateway-auth-native-import-data | closed authority requests/replies and result decoding; required-nullable fields, exact bounds | framing/proof/external native callers; native contracts, native wire/core/serde |
| gateway-auth-native-protocol | request/reply MAC frames, authenticated publication credential/request selection, durable replay adapter | native proof owner and public protocol fixtures; credentials, import DATA/contracts, primitives/native wire |
| gateway-auth-native-proof | NativeTransportIdentity + all inherent impls, actual HMAC verification, nonce consumption, owner/import/live private proof factories and currentness | native/authority/live/Worker external consumers; protocol/contracts/primitives; no reverse dependencies |
| gateway-auth-attestation | WebAuthn COSE/CBOR/certificate/CRL/ceremony and kernel verification policies | passkey service/resolver; contracts, primitives, selected crypto libraries |
| gateway-auth-passkeys | passkey writes/reads, origin/RP/CSRF, session revision index | resolver/authority; contracts, attestation, primitives; no contracts back-edge |
| gateway-auth-passport | RS256/JWKS cache verification and browser route service | resolver; contracts, persistent sessions, primitives; fixed authenticated effects |
| gateway-auth-passport-sessions | no-expiry durable local Passport authority, atomic replacement, central introspection, local-first revoke | browser/resolver; common AuthorityStore + finite AuthHost; contracts/primitives |
| gateway-auth | genuine resolve/bootstrap orchestration, method precedence, principal/admin/report | all external ordinary auth callers; leaves above |

Arrows in the last column point from consumer to defining dependency. Previously cyclic DATA is relocated before services:
passkey session/verification types precede session index and WebAuthn; Passport constants/claims/config precede upstream;
NativeTransportIdentity inherent impls stay in one crate, framing and DATA precede proofs. No proof constructor, serde,
key accessor, callback verifier, re-export, path attribute, include, suppression, facade or conditional hiding is introduced.
The native-publication selector remains authenticated policy (including real credential identity), never moved to M67 DATA.

## Exact ownership

Semantic: existing `rust/crates/gateway-auth/**`, new cohesive `rust/crates/gateway-auth-*` sources/manifests/tests;
M67 `gateway-auth-native-wire` stays unchanged absent demonstrated defect.
Worker shared semantic slices: `host.rs::impl WebAuthnCrypto`, `impl AuthHost` and fixed auth transport only;
`runtime.rs::auth_response`, `auth_response_with_native`, auth-report/Passport dispatch seams only.
New auth-only Worker helpers and `tools/cloudflare/runtime-matrix/auth-passport*`, `tools/cloudflare/fixtures/auth-passport*`.
External imports/manifests, root Cargo members/edges/first-party lock records, selector/ratchet/DECISIONS registration form a
separate mechanical diff and hash manifest. No external runtime semantics under an import repair.

## Preserved behavior and invariants

- Secret redaction and exact API-key/hash/revision/expiry semantics; original credential/generation/private projection identity.
- Fixed provider waterfall, one admission/settlement, actual original request/cancellation/store/native identity.
- WebAuthn actual signature/certificate/CBOR and source early-root/CRL policy, ordinary unauthenticated uncapped CRL redirects.
- Native exact HMAC bytes, body/request/purpose/capability/currentness, nonce consumption after MAC and no live-fence eviction.
- Source session revision digest domain and tuple keys, clock/revision rechecks after awaits, atomic paired source/index writes.
- Passport fixed issuer/client/audience/origin/callback. Production host HTTPS or HTTP with exact x-forwarded-proto=https;
  existing explicit constructor candidate composition remains separate from caller-selected authorization.
- PKCE nonce/verifier each 32 random bytes, state nonce.admin/chat, S256, two exact 600-second transient cookies.
- Callback duplicate/state/verifier/code checks before exchange; appSession:true, codeVerifier, fixed finite request,
  10s/64KiB/no redirects, RS256/claims plus agent rejection before durable creation. Failures clear transient cookies only.
- Persistent token/key/grant/record domains exactly as source; no expiry or role cache. Atomic absent-key CAS and optional
  prior deletion; failed creation preserves prior authority; revoke prior app grant only after commit, best effort.
- Every use introspects app bearer centrally, exact subject/binding/createdAt/user/role/scope validation; 401 deletes even
  if body cancellation fails, temporary/malformed/store failures are 503 preserving recoverable authority; reread after
  successful introspection lets logout/replacement win without republishing.
- Persistent identity uses passport_session, auth:sso, exp/jti/agent null; strict legacy JWT expiry stays required.
  Cookie-only current-admin permission, no super-admin, unchanged mutation CSRF, source precedence and audit actor.
- 400-day browser cookie renewed only on successful existing auth report, never a server TTL; only authoritative 401 clears.
- POST-only logout checks Origin/fetch-site first, deletes locally before independent best-effort revoke, truthful 204 header;
  store failure is 503 and cannot claim deletion. Repeated cookies, no-store and referrer headers preserved.

## Verification and handback plan

Before each gate record exact changed paths and smallest relevant closure. Every compiler/build/Clippy/test-driver/Worker
build holds `flock -x /tmp/ai-rust-cloud-control/compiler.lock <exact argv>`. Public prepared tools are read-only inputs;
no first-party target sharing. Select pinned Rust/Cargo 1.98.1, Bun 1.4.0, wasm target, worker-build 0.8.7,
wasm-bindgen 0.2.129; no lock regeneration or third-party version/checksum/feature changes. Preserve all profiles.
Use parser-backed structural inventory including conventional transitive test/support closures, then locked offline
portable x86_64 checks, exact staged strict Clippy and comparator, positive/nonzero focused auth tests behind supported
real OS fixture boundary, selected WASM closure and compiled Worker/common-authority controlled fixed-upstream fixtures.
All TypeScript fixtures remain offline/credential-free with actual confinement; no TEST_SCOPE_FORCE or whole Bun suite.
New tests cover the full Passport matrix and existing native capability/crypto negatives, not just one login.
Mac native/custody/descriptor, installed-client/browser and final combined acceptance remain controller-owned gates.
Feature, mechanical patch and ancestry-composed candidate are distinct refs/commits; return exact SHA/tree/counts,
receipts including failures/skips, separately hashed bundle/diff and independent restore/ref/tree proof.

## Unknowns, owner reports and stop conditions

- Shared filesystem filled during an attempted public tool copy; removed only incomplete task-owned copies. Use existing
  public tools read-only pending capacity, never delete other owners' caches. Input arrival is filesystem-event driven.
- Reduced-state migration: `core_state.rs::decide` broadly includes `[uos_ai,auth]`, but
  `projection.rs::validate_record` has only passkey-specific auth branches and treats auth as a generally known family.
  New Passport records require the migration owner's explicit lossless schema validation (key digest, no expiry,
  version/binding/grant domain). No migration body is owned here; do not silently assert validated adoption parity.
- Source UTF16 and Date.parse domains require lossless carriers and the actual JS date primitive in Worker, not an ISO-only
  parser. Trace downstream String principal/audit actor constraints before changing public identity; a required non-auth
  semantic adaptation must be reported with defining symbols before dependent changes.
- Material authority/capability change, private-factory cycle, missing authenticated effect boundary, non-auth body need,
  or lost input ancestry stops only the dependent change. Continue independent auth work and return exact owner/action.
- Material architecture changes increment this version and mark affected evidence stale. Authoring, command execution,
  component passes, candidate composition and integration acceptance remain distinct claims.
