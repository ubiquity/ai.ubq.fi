# Additional frozen-source key-domain findings

The actor proposal v1 is unchanged. These are auth-private corrections, not external foundation writes.
At b592 `passport.ts::verifyToken` requires JWT parts to match `[A-Za-z0-9_-]+` (no padding),
header kid to be a nonempty source string of at most 200 UTF16 units, and alg/typ to equal RS256/JWT.
`loadJwks` applies the same kid bound and exact source string equality/duplicate detection. A kid is
not an ASCII digest, UUID or native transport key ID. Lone units and 200 non-ASCII BMP units are
source-accepted kid data when the matching JWKS key is otherwise valid.

Accordingly the auth-private JWKS cache/lookup now uses JsString16, parses header/JWKS through
SourceJson, rejects present non-RS256/non-sig alg/use values (including null or non-string), and
rejects padded/non-base64url JWT parts before fetching keys. An authoritative valid JWKS lacking
kid is an invalid token (401); a failed/malformed key service remains unavailable (502). The
classification helper retains source `typeof header.alg === "string"` without an invented 8 KiB
header admission bound. Real RSA fixtures cover lossless/200-unit key IDs, unknown kid and padding.

Legacy PassportClaims keeps required exp/jti; its scalar strings now measure UTF16 units and iat/exp
must be source safe integers. The external legacy SourceActor representation, minting and checks
are unchanged. This does not resolve the separately reported legacy/app identity carrier seams.
Cookie selection has one defining browser helper: duplicate Passport cookie names return absence,
while a single empty value is preserved for source authentication failure. No forwarding export or
public proof constructor was introduced. All new fixture definitions still require execution gates.
