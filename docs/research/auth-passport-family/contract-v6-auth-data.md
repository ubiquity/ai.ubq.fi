# Contract v6 — complete auth-method lossless DATA seam

Extends the frozen v5 foundation without authority semantic writes. AuthMethod remains defined in
`gateway-auth-contracts::types`. `PassportSession { identity: PassportSessionData }` distinguishes mandatory JWT
claims from persistent app identity+source binding; `PassportBearer { claims: PassportSourceClaims }` preserves all
source-valid subject/jti/handle/auth/scope units. `principal_id` returns the explicit Core PrincipalId.

Historical AuthMethod scalar serde retains exactly its old kind/claims shape through a private finite adapter to the
unchanged PassportClaims DATA. Its scalar conversion is only this proven old codec boundary, never authentication or
an identity substitute. App/source v2 actors use explicit v2 DATA; an app session refuses historical JWT serialization.
There is no fake jti/expiry, public proof constructor, cached role or new public wire kind. Contracts remains bounded.
The actual legacy SourceActor and v1 native bytes are untouched. SourceAuthMethod consumers are exact named resolver,
audit and live principal producers, plus root-owned authority/native currentness; the latter remain pending assigned
owner integration. New internal DATA field names require defining consumer adaptation before full candidate acceptance.

Dependent resolver/browser/report integration follows this separate DATA/API commit. Prior scalar v2 auth/report
receipts become stale; the earlier component receipts retain their exact source scope only. Fixed Date.parse primitive,
source record/revision/token binding and fresh AuthService operation from v5 are unchanged.
