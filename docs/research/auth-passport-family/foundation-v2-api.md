# Frozen Passport foundation DATA/API v2

The accepted root decision is hashed in contract v5 and the exact API/consumer list is
`foundation-v2-api-consumers.json`. This commit supplies DATA and a concrete checked AuthService operation.
It does not implement Authority actor/native semantic bodies or establish integration acceptance.

PrincipalId preserves exact source units, equality and hashing. `from_scalar` retains the old infallible scalar
constructor domain; existing caller validation still applies. `from_source` is explicit. Default serde retains
historical scalar bytes and refuses lone units; the named source codecs preserve those units. No Display, lossy
conversion or replacement identity is supplied. Only the existing tuple boundary applies source TextEncoder behavior.
Other opaque IDs keep their original fields and guards.

`PassportAppSessionBinding` contains the local SHA256 digest, exact row revision, exact subject/createdAt, fixed four
binding fields and app-token SHA256 digest. It contains no token, role, scope or expiry. The source row's expiry=None
means no server TTL, never current authority. `PassportActorDataV2` is version=2 with explicit app-session and lossless
JWT variants; JWT sub/jti and expiry are mandatory. Historical scalar native JWT v1 remains unchanged. The new v2
source-valid JWT form closes the DATA representation question; new authentication integration must use lossless claims
and the authority owner must select v2 explicitly rather than broaden old v1 decoding.

`AuthService::check_passport_app_session` requires the constructor-held store and AuthHost+PassportDate. It reads the
actual no-expiry source row and its app token, compares every binding field, performs the fixed upstream operation
outside any Authority transaction, validates current central identity/role/scopes, then rereads the exact revision and
all bindings. 503 preserves state; authoritative401 deletes local authority. This output is ordinary DATA. Authority
must mint its own private non-Clone/non-Deserialize single-use value through this concrete operation and bind its final
effect transaction/CAS to that row. Each effect/native recheck after an await repeats the fresh operation; a store-only
reader must refuse v2 when that capability is absent. No time-only check, generic callback or serialized success flag
satisfies this boundary. Production Worker Date parsing uses actual JS Date.parse with a UTF16 JS string; unsupported
hosts need that concrete source primitive, never an ISO fallback.

Audit nullable identity/label and live mapping principal DATA use lossless source carriers. The existing Json/ProfileV2
codec retains scalar historical rows and source lone units; other fields, TTL, retirement, source ordering, and lifecycle
are unchanged. Current auth-method integration follows this freeze. The current admin defining path comes from the
exact admin ancestor; the semantic DATA delta transfers only to that defining audit module in composition.

Pending semantic ownership is explicit in the JSON map: Authority actor/currentness/final-CAS belongs to
rust_authority_size; native receiver/fence/effect handling to rust_migration; custody adapters await their root-assigned
writer. No authority semantic file was edited here. Private v2 fence/response/MAC integration must consume this exact
version/lossless framing and refuse version confusion while preserving old v1 selectors/captures. These are remaining
owner integration gates, not a waiver or evidence of passed authority/native acceptance.
