// Ubiquity Passport caller authentication. URLs are fixed constants; only a
use gateway_auth_passport_contracts::passport::{
    PASSPORT_AUTHORIZE_URL, PASSPORT_COOKIE, PASSPORT_EXCHANGE_MAX_BYTES, PASSPORT_JWKS_CACHE_MS,
    PASSPORT_JWKS_MAX_BYTES, PASSPORT_STATE_COOKIE, PASSPORT_STATE_TTL_SECONDS, PassportClaims,
    PassportConfig,
};
// separately registered preview client may vary its public origin and redirect.
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthRequest, AuthResponse, AuthResponseBody},
    upstream::AuthUpstreamRequest,
};
use gateway_auth_primitives::{
    crypto::{Secret, base64url, decode_base64url},
    error::unauthorized,
};
use gateway_core::error::GatewayError;
use rsa::{
    BigUint, RsaPublicKey,
    pkcs1v15::{Signature, VerifyingKey},
    signature::{SignatureEncoding, Verifier},
    traits::PublicKeyParts,
};
use serde_json::{Value, json};
use sha2::Sha256;
use std::{cell::RefCell, collections::BTreeMap};
use url::Url;

pub fn config_for_request(config: &AuthConfig, request: &AuthRequest) -> Option<PassportConfig> {
    let explicit = config.passport.clone();
    let request_url = Url::parse(&request.url).ok()?;
    let host = request_url.host_str()?.trim_matches(['[', ']']);
    let loopback = host.eq_ignore_ascii_case("localhost")
        || host
            .parse::<std::net::IpAddr>()
            .is_ok_and(|address| address.is_loopback());
    if let Some(passport) = explicit {
        let request_origin = request_url.origin().ascii_serialization();
        return (loopback || request_origin == passport.origin).then_some(passport);
    }
    (request_url.scheme() == "https" && host == "ai.ubq.fi").then(PassportConfig::production)
}

thread_local! {
    static JWKS_CACHE: RefCell<Option<(i64, BTreeMap<String, RsaPublicKey>)>> = const { RefCell::new(None) };
}

/// Test isolation only: production code never calls this.
pub fn reset_jwks_cache() {
    JWKS_CACHE.with(|cache| *cache.borrow_mut() = None);
}

fn invalid_token() -> GatewayError {
    unauthorized()
}

fn passport_unavailable() -> GatewayError {
    GatewayError::new(
        502,
        "Passport verification is unavailable.",
        Some("passport_unavailable"),
    )
}

pub fn jwt_shaped(token: &str) -> bool {
    let parts = token.split('.').collect::<Vec<_>>();
    if parts.len() != 3 || parts.iter().any(|part| part.is_empty()) || parts[0].len() > 8_192 {
        return false;
    }
    decode_base64url(parts[0])
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .is_some_and(|header| header.get("alg").and_then(Value::as_str).is_some())
}

fn parse_raw_jwt(
    token: &str,
) -> Result<
    (
        Value,
        gateway_core::source_json::SourceJson,
        String,
        Signature,
    ),
    GatewayError,
> {
    if token.len() > 32_768 {
        return Err(invalid_token());
    }
    let parts = token.split('.').collect::<Vec<_>>();
    if parts.len() != 3 || parts.iter().any(|part| part.is_empty()) {
        return Err(invalid_token());
    }
    let header: Value =
        serde_json::from_slice(&decode_base64url(parts[0]).map_err(|_| invalid_token())?)
            .map_err(|_| invalid_token())?;
    if !header.is_object()
        || header.get("alg").and_then(Value::as_str) != Some("RS256")
        || header.get("typ").and_then(Value::as_str) != Some("JWT")
    {
        return Err(invalid_token());
    }
    if ["jku", "x5u", "x5c", "jwk", "crit"]
        .iter()
        .any(|name| header.get(*name).is_some())
    {
        return Err(invalid_token());
    }
    let kid = header
        .get("kid")
        .and_then(Value::as_str)
        .filter(|kid| !kid.is_empty() && kid.len() <= 200)
        .ok_or_else(invalid_token)?
        .to_owned();
    let claims = gateway_auth_passport_contracts::session::parse_source_json(
        &decode_base64url(parts[1]).map_err(|_| invalid_token())?,
    )
    .map_err(|_| invalid_token())?;
    let signature = Signature::try_from(
        decode_base64url(parts[2])
            .map_err(|_| invalid_token())?
            .as_slice(),
    )
    .map_err(|_| invalid_token())?;
    Ok((header, claims, kid, signature))
}

pub fn verify_jwt(
    token: &str,
    config: &PassportConfig,
    now_seconds: i64,
    keys: &BTreeMap<String, RsaPublicKey>,
) -> Result<PassportClaims, GatewayError> {
    let (_, claims, kid, signature) = parse_raw_jwt(token)?;
    let claims: PassportClaims = claims.deserialize().map_err(|_| invalid_token())?;
    let key = keys.get(&kid).ok_or_else(invalid_token)?;
    if signature.to_vec().len() != key.size() {
        return Err(invalid_token());
    }
    let parts = token.split('.').collect::<Vec<_>>();
    let input = format!("{}.{}", parts[0], parts[1]);
    if VerifyingKey::<Sha256>::new(key.clone())
        .verify(input.as_bytes(), &signature)
        .is_err()
        || !claims.valid(config, now_seconds)
    {
        return Err(invalid_token());
    }
    Ok(claims)
}

fn jwks_keys(body: &[u8]) -> Result<BTreeMap<String, RsaPublicKey>, GatewayError> {
    if body.len() > PASSPORT_JWKS_MAX_BYTES {
        return Err(passport_unavailable());
    }
    let document: Value = serde_json::from_slice(body).map_err(|_| passport_unavailable())?;
    let keys = document
        .get("keys")
        .and_then(Value::as_array)
        .filter(|keys| !keys.is_empty())
        .ok_or_else(passport_unavailable)?;
    let mut parsed = BTreeMap::new();
    for key in keys {
        if key.get("kty").and_then(Value::as_str) != Some("RSA")
            || key
                .get("alg")
                .and_then(Value::as_str)
                .is_some_and(|alg| alg != "RS256")
            || key
                .get("use")
                .and_then(Value::as_str)
                .is_some_and(|usage| usage != "sig")
        {
            continue;
        }
        let kid = key
            .get("kid")
            .and_then(Value::as_str)
            .filter(|kid| !kid.is_empty() && kid.len() <= 200)
            .ok_or_else(passport_unavailable)?;
        if parsed.contains_key(kid) {
            return Err(passport_unavailable());
        }
        let modulus = key
            .get("n")
            .and_then(Value::as_str)
            .and_then(|value| decode_base64url(value).ok())
            .filter(|bytes| (256..=1_024).contains(&bytes.len()))
            .ok_or_else(passport_unavailable)?;
        let exponent = key
            .get("e")
            .and_then(Value::as_str)
            .and_then(|value| decode_base64url(value).ok())
            .filter(|bytes| !bytes.is_empty() && bytes.len() <= 8)
            .ok_or_else(passport_unavailable)?;
        let key = RsaPublicKey::new(
            BigUint::from_bytes_be(&modulus),
            BigUint::from_bytes_be(&exponent),
        )
        .map_err(|_| passport_unavailable())?;
        parsed.insert(kid.to_owned(), key);
    }
    if parsed.is_empty() {
        return Err(passport_unavailable());
    }
    Ok(parsed)
}

async fn cached_jwks<H: AuthHost>(
    host: &H,
    now_ms: i64,
    unknown_kid: Option<&str>,
) -> Result<BTreeMap<String, RsaPublicKey>, GatewayError> {
    let cached = JWKS_CACHE.with(|cache| {
        cache
            .borrow()
            .as_ref()
            .filter(|(fetched_at, _)| now_ms.saturating_sub(*fetched_at) < PASSPORT_JWKS_CACHE_MS)
            .map(|(_, keys)| keys.clone())
    });
    if let Some(keys) = cached
        && unknown_kid.is_none_or(|kid| keys.contains_key(kid))
    {
        return Ok(keys);
    }
    let response = host
        .fetch_auth(AuthUpstreamRequest::PassportJwks)
        .await
        .map_err(|_| passport_unavailable())?;
    if !response.success() || response.body.len() > PASSPORT_JWKS_MAX_BYTES {
        return Err(passport_unavailable());
    }
    let keys = jwks_keys(&response.body)?;
    JWKS_CACHE.with(|cache| *cache.borrow_mut() = Some((now_ms, keys.clone())));
    if unknown_kid.is_some_and(|kid| !keys.contains_key(kid)) {
        return Err(passport_unavailable());
    }
    Ok(keys)
}

pub async fn verify<H: AuthHost>(
    host: &H,
    config: &PassportConfig,
    token: &str,
    now_ms: i64,
) -> Result<PassportClaims, GatewayError> {
    let (_, _, kid, _) = parse_raw_jwt(token)?;
    let keys = cached_jwks(host, now_ms, Some(&kid)).await?;
    verify_jwt(token, config, now_ms.div_euclid(1000), &keys)
}

pub fn cookie(request: &AuthRequest, name: &str) -> Option<String> {
    for part in request.header("cookie")?.split(';') {
        let Some((key, value)) = part.split_once('=') else {
            continue;
        };
        if key.trim() == name {
            let value = value.trim();
            return (!value.is_empty()).then(|| value.to_owned());
        }
    }
    None
}

fn session_cookie(token: &str, max_age: i64) -> String {
    format!(
        "{PASSPORT_COOKIE}={token}; Path=/; Max-Age={}; HttpOnly; Secure; SameSite=Lax",
        max_age.max(0)
    )
}
fn state_cookie(state: &str) -> String {
    format!(
        "{PASSPORT_STATE_COOKIE}={state}; Path=/; Max-Age={PASSPORT_STATE_TTL_SECONDS}; HttpOnly; Secure; SameSite=Lax"
    )
}
fn clear_state_cookie() -> String {
    format!("{PASSPORT_STATE_COOKIE}=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Lax")
}
fn clear_session_cookie() -> String {
    format!("{PASSPORT_COOKIE}=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Lax")
}

pub struct PassportService<'a, H> {
    pub host: &'a H,
    pub config: &'a PassportConfig,
}

impl<'a, H: AuthHost> PassportService<'a, H> {
    pub async fn session(
        &self,
        request: &AuthRequest,
    ) -> Result<Option<PassportClaims>, GatewayError> {
        let Some(token) = cookie(request, PASSPORT_COOKIE) else {
            return Ok(None);
        };
        verify(self.host, self.config, &token, self.host.now_ms())
            .await
            .map(Some)
    }

    pub async fn bearer(&self, token: &str) -> Result<PassportClaims, GatewayError> {
        verify(self.host, self.config, token, self.host.now_ms()).await
    }

    pub async fn login(&self, request: &AuthRequest) -> Result<AuthResponse, GatewayError> {
        if request.method != "GET" {
            return Err(GatewayError::new(
                405,
                "Method not allowed",
                Some("method_not_allowed"),
            ));
        }
        let random = self.host.random_bytes(32)?;
        if random.len() != 32 {
            return Err(GatewayError::unavailable(
                "Random source returned the wrong length",
                "auth_random_unavailable",
            ));
        }
        let state = base64url(&random);
        let mut target = Url::parse(PASSPORT_AUTHORIZE_URL).map_err(|_| passport_unavailable())?;
        target
            .query_pairs_mut()
            .append_pair("client_id", &self.config.client_id)
            .append_pair("audience", &self.config.audience)
            .append_pair("origin", &self.config.origin)
            .append_pair("redirect_uri", &self.config.redirect_uri)
            .append_pair("state", &state);
        let mut response = AuthResponse {
            status: 302,
            headers: BTreeMap::from([("location".into(), target.to_string())]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![state_cookie(&state)],
            stream: None,
        };
        response
            .headers
            .insert("cache-control".into(), "no-store".into());
        Ok(response)
    }

    pub async fn callback(&self, request: &AuthRequest) -> Result<AuthResponse, GatewayError> {
        if request.method != "GET" {
            return Err(GatewayError::new(
                405,
                "Method not allowed",
                Some("method_not_allowed"),
            ));
        }
        let fail = |error: GatewayError| {
            let mut response = AuthResponse::error(error);
            response.set_cookies.push(clear_state_cookie());
            Ok(response)
        };
        let url = match Url::parse(&request.url) {
            Ok(url) => url,
            Err(_) => return fail(unauthorized()),
        };
        let mut state = None;
        let mut code = None;
        for (key, value) in url.query_pairs() {
            match key.as_ref() {
                "state" if state.is_none() => state = Some(value.into_owned()),
                "code" if code.is_none() => code = Some(value.into_owned()),
                "state" | "code" => return fail(unauthorized()),
                _ => {}
            }
        }
        let state = state.filter(|value| !value.is_empty() && value.len() <= 200);
        let code = code.filter(|value| !value.is_empty() && value.len() <= 500);
        let (Some(state), Some(code)) = (state, code) else {
            return fail(unauthorized());
        };
        if cookie(request, PASSPORT_STATE_COOKIE).as_deref() != Some(state.as_str()) {
            return fail(unauthorized());
        }
        let exchange = match self
            .host
            .fetch_auth(AuthUpstreamRequest::PassportExchange {
                code: Secret::new(code),
                client_id: self.config.client_id.clone(),
                audience: self.config.audience.clone(),
                origin: self.config.origin.clone(),
                redirect_uri: self.config.redirect_uri.clone(),
            })
            .await
        {
            Ok(exchange) => exchange,
            Err(_) => return fail(passport_unavailable()),
        };
        if !exchange.success() || exchange.body.len() > PASSPORT_EXCHANGE_MAX_BYTES {
            return fail(unauthorized());
        }
        let body: Value = match serde_json::from_slice(&exchange.body) {
            Ok(body) => body,
            Err(_) => return fail(unauthorized()),
        };
        let Some(token) = body
            .get("accessToken")
            .and_then(Value::as_str)
            .filter(|token| !token.is_empty() && token.len() <= 32_768)
        else {
            return fail(unauthorized());
        };
        let claims = match verify(self.host, self.config, token, self.host.now_ms()).await {
            Ok(claims) => claims,
            Err(error) => return fail(error),
        };
        let max_age = claims
            .exp
            .saturating_sub(self.host.now_ms().div_euclid(1000));
        Ok(AuthResponse {
            status: 302,
            headers: BTreeMap::from([
                ("location".into(), "/chat".into()),
                ("cache-control".into(), "no-store".into()),
            ]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![session_cookie(token, max_age), clear_state_cookie()],
            stream: None,
        })
    }

    pub fn logout(&self) -> AuthResponse {
        AuthResponse {
            status: 204,
            headers: BTreeMap::from([("cache-control".into(), "no-store".into())]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![clear_session_cookie()],
            stream: None,
        }
    }
}

pub fn method_json(claims: &PassportClaims, bearer: bool) -> Value {
    json!({
        "kind": if bearer { "passport_bearer" } else { "passport_session" },
        "principal": format!("passport-user:{}", claims.sub),
        "user": {
            "sub": claims.sub,
            "handle": claims.handle,
            "is_admin": claims.is_admin,
            "scopes": claims.scopes,
            "auth": claims.auth,
            "exp": claims.exp,
            "jti": claims.jti,
            "agent_token_id": claims.agent_token_id,
        }
    })
}

/// Verify source-permitted UTF16 fields with the same real RS256/JWKS policy.
pub async fn verify_source<H: AuthHost>(
    host: &H,
    token: &str,
) -> Result<gateway_auth_passport_contracts::session::PassportSourceClaims, GatewayError> {
    let (_, document, kid, signature) = parse_raw_jwt(token)?;
    let now = host.now_ms();
    let keys = cached_jwks(host, now, Some(&kid)).await?;
    let key = keys.get(&kid).ok_or_else(invalid_token)?;
    let claims: gateway_auth_passport_contracts::session::PassportSourceClaims =
        document.deserialize().map_err(|_| invalid_token())?;
    let parts = token.split('.').collect::<Vec<_>>();
    let input = format!("{}.{}", parts[0], parts[1]);
    if signature.to_vec().len() != key.size()
        || !claims.valid(host.now_ms().div_euclid(1000))
        || VerifyingKey::<Sha256>::new(key.clone())
            .verify(input.as_bytes(), &signature)
            .is_err()
    {
        return Err(invalid_token());
    }
    Ok(claims)
}
