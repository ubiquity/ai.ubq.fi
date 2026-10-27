# auth-passport-family contract v3

Extends [v2](contract-v2.md). Input source, identity, authority limits and acceptance remain unchanged.
All v2 architecture/count/compile evidence affected by these changes is stale until rerun.

## Source-derived architecture corrections

- `AuthConfig::from_source_values` is an inherent implementation. Its defining `bootstrap` module moves with
  AuthConfig into gateway-auth-contracts, rather than leaving an illegal foreign inherent impl in the resolver crate.
- Strict expiring `PassportClaims` remains the existing JWT consumer DTO. New `PassportSourceClaims` retains source
  UTF16 and mandatory safe-integer expiration for the new callback, verified by the same real RSA/JWKS policy.
  Persistent `PassportSessionIdentity` remains separate data, with no JWT/expiration/sentinel constructor.
- `PassportDate::parse_date_ms` is only the platform Date.parse primitive; Worker passes actual UTF16 to its JS engine.
  No ISO-only Rust parser is substituted. Finite portable fixtures list their explicit date observations.
- Browser PKCE/session ceremonies and durable app-session operations are independently authored with public behavior
  fixtures. They are not switched into the shipping resolver until the external effect/identity owner boundary is resolved.
  Returning persistent cookies while ordinary auth cannot consume them would publish a broken login flow.

## Exact outstanding owner boundary

`gateway-authority/src/native_control/actor.rs` matches AuthMethod::PassportSession and requires
`SourceActor::Passport { sub: claims.sub.clone(), jti: claims.jti.clone(), expires_at_ms:
claims.exp.checked_mul(1000).ok_or_else(fail)? }`. Its native renewal actor is current, bounded effect authority.
Persistent Passport source identity has exp:null and jti:null. Neither a sentinel nor optional JWT expiration is permitted.
The authority owner must define the persistent, current-authority actor/effect boundary before auth integrates that identity.
Auth does not widen SourceActor, forge proof, add a callback verifier, fabricate a JWT or own that external runtime body.

Additional external scalar boundary: `AuthMethod::principal_id -> String`, `gateway-admin::audit::ChangeActor` scalar
principal/label, `gateway-live-runtime::protocol` and the authority actor store assume scalar subject/handle Strings.
Lossless persistent subject/handle may contain lone UTF16 units. The owner integration must retain them, not replace,
escape, hash, refuse or silently narrow source values. This is semantic work beyond defining-import repairs.

Migration owner report remains: core_state broadly selects `[uos_ai,auth]`; projection has no exact Passport session
schema branch. No imported/live state was read and no migration body was changed.

## Platform blockers and evidence

- Actual x86_64 portable production compile has succeeded. This is not a native execution proof.
- The shared filesystem repeatedly exhausted space during builds; the first compile failed with ENOSPC and linker signal 7.
  Only task-owned incomplete tool copies were deleted. Public prepared tools/registry archives are used read-only;
  the task's Cargo index, temporary directory and first-party target are isolated. No O1 target cache is shared.
- The existing Linux namespace fixture boundary fails: `unshare: cannot open /proc/self/uid_map: Read-only file system`.
  No fixture is run outside real confinement. Tests may be compiled but must not be reported as executed/passed.
- Compiled Worker/common-authority runtime conformance, Mac/native, installed browser/client and final source integration
  are not accepted by portable checks or mock tests. Controller/host must supply the supported fixture boundary and space.

The handback will explicitly distinguish independent authored/compiled work from the blocked identity/effect integration,
executed tests and whole-family acceptance. No architecture or gate is silently lowered.
