//! Ubiquity Passport caller authentication. URLs are fixed constants; only a
//! separately registered preview client may vary its public origin and redirect.
use crate::{
    crypto::{Secret, base64url, decode_base64url},
    types::{AuthConfig, AuthHost, AuthRequest, AuthResponse, AuthResponseBody, unauthorized},
    upstream::AuthUpstreamRequest,
};
use gateway_core::{error::GatewayError, state::AuthorityStore};
use gateway_passport_session::{
    binding::{
        Binding, PASSPORT_COOKIE_MAX_AGE_SECONDS, is_persistent_session, is_random43,
        pkce_challenge,
    },
    call::{SessionCall, SessionHost, SessionReply},
    service::{Identity, PassportSessions},
};
use rsa::{
    BigUint, RsaPublicKey,
    pkcs1v15::{Signature, VerifyingKey},
    signature::{SignatureEncoding, Verifier},
    traits::PublicKeyParts,
};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use sha2::Sha256;
use std::{cell::RefCell, collections::BTreeMap};
use url::Url;

pub const PASSPORT_ISSUER: &str = "https://passport.ubq.fi";
pub const PASSPORT_AUTHORIZE_URL: &str = "https://passport.ubq.fi/authorize";
pub const PASSPORT_EXCHANGE_URL: &str = "https://passport.ubq.fi/api/auth/sso/exchange";
pub const PASSPORT_JWKS_URL: &str = "https://passport.ubq.fi/api/auth/jwks";
pub const PASSPORT_PRODUCTION_CLIENT_ID: &str = "ai";
pub const PASSPORT_PRODUCTION_AUDIENCE: &str = "ai";
pub const PASSPORT_PRODUCTION_ORIGIN: &str = "https://ai.ubq.fi";
pub const PASSPORT_PRODUCTION_REDIRECT_URI: &str = "https://ai.ubq.fi/auth/callback";
pub const PASSPORT_COOKIE: &str = "__Host-uos_ai_passport_session";
pub const PASSPORT_STATE_COOKIE: &str = "__Host-uos_ai_passport_state";
pub const PASSPORT_VERIFIER_COOKIE: &str = "__Host-uos_ai_passport_verifier";
pub const PASSPORT_STATE_TTL_SECONDS: i64 = 600;
pub const PASSPORT_JWKS_CACHE_MS: i64 = 300_000;
pub const PASSPORT_JWKS_MAX_BYTES: usize = 262_144;
pub const PASSPORT_EXCHANGE_MAX_BYTES: usize = 65_536;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PassportConfig {
    pub client_id: String,
    pub audience: String,
    pub origin: String,
    pub redirect_uri: String,
}

impl PassportConfig {
    pub fn production() -> Self {
        Self {
            client_id: PASSPORT_PRODUCTION_CLIENT_ID.into(),
            audience: PASSPORT_PRODUCTION_AUDIENCE.into(),
            origin: PASSPORT_PRODUCTION_ORIGIN.into(),
            redirect_uri: PASSPORT_PRODUCTION_REDIRECT_URI.into(),
        }
    }

    /// Explicit preview registration only. Partial or non-HTTPS values disable
    /// browser login rather than silently reusing the production `ai` client.
    pub fn from_values(values: &BTreeMap<String, String>) -> Option<Self> {
        let value = |name: &str| {
            values
                .get(name)
                .map(|value| value.trim())
                .filter(|value| !value.is_empty())
        };
        let client_id = value("UOS_AI_PASSPORT_CLIENT_ID")?;
        let audience = value("UOS_AI_PASSPORT_AUDIENCE").unwrap_or(PASSPORT_PRODUCTION_AUDIENCE);
        let origin = value("UOS_AI_PASSPORT_ORIGIN")?;
        let redirect_uri = value("UOS_AI_PASSPORT_REDIRECT_URI")?;
        if !valid_https_origin(origin) || client_id.len() > 120 || audience.len() > 160 {
            return None;
        }
        let redirect = Url::parse(redirect_uri).ok()?;
        if redirect.scheme() != "https"
            || redirect.origin().ascii_serialization() != origin.trim_end_matches('/')
            || redirect.query().is_some()
            || redirect.fragment().is_some()
        {
            return None;
        }
        Some(Self {
            client_id: client_id.into(),
            audience: audience.into(),
            origin: origin.trim_end_matches('/').into(),
            redirect_uri: redirect_uri.into(),
        })
    }

    fn binding(&self) -> Binding {
        Binding {
            client_id: self.client_id.clone(),
            audience: self.audience.clone(),
            origin: self.origin.clone(),
            redirect_uri: self.redirect_uri.clone(),
        }
    }
}

fn valid_https_origin(value: &str) -> bool {
    let Ok(url) = Url::parse(value) else {
        return false;
    };
    url.scheme() == "https"
        && url.port().is_none()
        && url.host_str().is_some()
        && url.path() == "/"
        && url.query().is_none()
        && url.fragment().is_none()
        && url.origin().ascii_serialization() == value.trim_end_matches('/')
}

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

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct PassportClaims {
    pub iss: String,
    pub sub: String,
    #[serde(default)]
    pub handle: String,
    #[serde(rename = "isAdmin")]
    pub is_admin: bool,
    #[serde(default)]
    pub scopes: Vec<String>,
    pub aud: String,
    #[serde(rename = "clientId")]
    pub client_id: String,
    pub auth: String,
    /// Access-token lifetime and id. A persistent app session has neither:
    /// its identity is Passport's current user, introspected on this request.
    pub iat: Option<i64>,
    pub exp: Option<i64>,
    pub jti: Option<String>,
    #[serde(
        rename = "agentTokenId",
        default,
        skip_serializing_if = "Option::is_none"
    )]
    pub agent_token_id: Option<String>,
}

impl PassportClaims {
    fn persistent(identity: Identity, config: &PassportConfig) -> Self {
        Self {
            iss: PASSPORT_ISSUER.into(),
            sub: identity.subject,
            handle: identity.handle,
            is_admin: identity.is_admin,
            scopes: identity.scopes,
            aud: config.audience.clone(),
            client_id: config.client_id.clone(),
            auth: "sso".into(),
            iat: None,
            exp: None,
            jti: None,
            agent_token_id: None,
        }
    }

    fn valid(&self, config: &PassportConfig, now_seconds: i64) -> bool {
        let (Some(iat), Some(exp), Some(jti)) = (self.iat, self.exp, self.jti.as_deref()) else {
            return false;
        };
        self.iss == PASSPORT_ISSUER
            && self.aud == config.audience
            && self.client_id == config.client_id
            && !self.sub.is_empty()
            && self.sub.len() <= 500
            && !jti.is_empty()
            && jti.len() <= 500
            && !self.auth.is_empty()
            && self.auth.len() <= 160
            && self.handle.len() <= 500
            && self.scopes.len() <= 128
            && self.scopes.iter().all(|scope| scope.len() <= 500)
            && exp > now_seconds
            && iat <= now_seconds.saturating_add(60)
            && exp > iat
            && self
                .agent_token_id
                .as_ref()
                .is_none_or(|id| !id.is_empty() && id.len() <= 500)
    }
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

fn parse_jwt(token: &str) -> Result<(Value, PassportClaims, String, Signature), GatewayError> {
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
    if ["jku", "x5u", "x5c", "jwk"]
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
    let claims: PassportClaims =
        serde_json::from_slice(&decode_base64url(parts[1]).map_err(|_| invalid_token())?)
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
    let (_, claims, kid, signature) = parse_jwt(token)?;
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
    let (_, _, kid, _) = parse_jwt(token)?;
    let keys = cached_jwks(host, now_ms, Some(&kid)).await?;
    verify_jwt(token, config, now_ms.div_euclid(1000), &keys)
}

/// Exactly one non-empty value; an ambiguous duplicate is no credential.
pub fn cookie(request: &AuthRequest, name: &str) -> Option<String> {
    let mut values = request
        .header("cookie")?
        .split(';')
        .filter_map(|part| part.split_once('='))
        .filter(|(key, _)| key.trim() == name)
        .map(|(_, value)| value.trim());
    let value = values.next()?;
    (values.next().is_none() && !value.is_empty()).then(|| value.to_owned())
}

fn host_cookie(name: &str, value: &str, max_age: i64) -> String {
    format!("{name}={value}; Path=/; Max-Age={max_age}; HttpOnly; Secure; SameSite=Lax")
}
/// The persistent session cookie, renewed on each successful auth report.
pub fn session_cookie(token: &str) -> String {
    host_cookie(PASSPORT_COOKIE, token, PASSPORT_COOKIE_MAX_AGE_SECONDS)
}
pub fn clear_session_cookie() -> String {
    host_cookie(PASSPORT_COOKIE, "", 0)
}
fn transient_cookies(state: &str, verifier: &str, max_age: i64) -> [String; 2] {
    [
        host_cookie(PASSPORT_STATE_COOKIE, state, max_age),
        host_cookie(PASSPORT_VERIFIER_COOKIE, verifier, max_age),
    ]
}
/// A persistent session whose authority was authoritatively removed.
pub fn revoked_session(request: &AuthRequest, error: &GatewayError) -> bool {
    error.status == 401
        && cookie(request, PASSPORT_COOKIE).is_some_and(|token| is_persistent_session(&token))
}
/// Source state: 32 random bytes bound to one fixed local destination.
fn valid_state(state: &str) -> bool {
    state
        .split_once('.')
        .is_some_and(|(random, target)| is_random43(random) && matches!(target, "admin" | "chat"))
}

struct SessionAdapter<'a, H>(&'a H);
impl<H: AuthHost> SessionHost for SessionAdapter<'_, H> {
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        self.0.random_bytes(length)
    }
    async fn fetch_session(&self, call: SessionCall) -> Result<SessionReply, GatewayError> {
        let reply = self
            .0
            .fetch_auth(AuthUpstreamRequest::PassportSession(call))
            .await?;
        Ok(SessionReply {
            status: reply.status,
            body: reply.body,
        })
    }
}

pub struct PassportService<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
    pub config: &'a PassportConfig,
}

impl<'a, S: AuthorityStore, H: AuthHost> PassportService<'a, S, H> {
    fn sessions<'b>(
        &'b self,
        binding: &'b Binding,
        adapter: &'b SessionAdapter<'a, H>,
    ) -> PassportSessions<'b, S, SessionAdapter<'a, H>> {
        PassportSessions {
            store: self.store,
            host: adapter,
            binding,
        }
    }

    pub async fn session(
        &self,
        request: &AuthRequest,
    ) -> Result<Option<PassportClaims>, GatewayError> {
        let Some(token) = cookie(request, PASSPORT_COOKIE) else {
            return Ok(None);
        };
        if is_persistent_session(&token) {
            let (binding, adapter) = (self.config.binding(), SessionAdapter(self.host));
            let identity = self.sessions(&binding, &adapter).get(&token).await?;
            return Ok(Some(PassportClaims::persistent(identity, self.config)));
        }
        verify(self.host, self.config, &token, self.host.now_ms())
            .await
            .map(Some)
    }

    fn random43(&self) -> Result<String, GatewayError> {
        let random = self.host.random_bytes(32)?;
        if random.len() != 32 {
            return Err(GatewayError::unavailable(
                "Random source returned the wrong length",
                "auth_random_unavailable",
            ));
        }
        Ok(base64url(&random))
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
        let url = Url::parse(&request.url).map_err(|_| unauthorized())?;
        let admin = url
            .query_pairs()
            .find(|(name, _)| name == "return_to")
            .is_some_and(|(_, value)| value == "/admin");
        let state = format!(
            "{}.{}",
            self.random43()?,
            if admin { "admin" } else { "chat" }
        );
        let verifier = self.random43()?;
        let mut target = Url::parse(PASSPORT_AUTHORIZE_URL).map_err(|_| passport_unavailable())?;
        target
            .query_pairs_mut()
            .append_pair("client_id", &self.config.client_id)
            .append_pair("audience", &self.config.audience)
            .append_pair("origin", &self.config.origin)
            .append_pair("redirect_uri", &self.config.redirect_uri)
            .append_pair("state", &state)
            .append_pair("code_challenge", &pkce_challenge(&verifier))
            .append_pair("code_challenge_method", "S256");
        let mut response = AuthResponse {
            status: 302,
            headers: BTreeMap::from([("location".into(), target.to_string())]),
            body: AuthResponseBody::Empty,
            set_cookies: transient_cookies(&state, &verifier, PASSPORT_STATE_TTL_SECONDS).into(),
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
        // A failed callback clears only transient cookies, keeping any session.
        let fail = |error: GatewayError| {
            let mut response = AuthResponse::error(error);
            response.set_cookies.extend(transient_cookies("", "", 0));
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
        let state = state.filter(|value| valid_state(value));
        let code = code.filter(|value| !value.is_empty() && value.len() <= 500);
        let verifier = cookie(request, PASSPORT_VERIFIER_COOKIE).filter(|value| is_random43(value));
        let (Some(state), Some(code), Some(verifier)) = (state, code, verifier) else {
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
                code_verifier: Secret::new(verifier),
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
            Ok(claims) if claims.agent_token_id.is_none() => claims,
            Ok(_) => return fail(unauthorized()),
            Err(error) => return fail(error),
        };
        // Only a fresh code exchange creates a grant; a legacy JWT never does.
        let (binding, adapter) = (self.config.binding(), SessionAdapter(self.host));
        let previous = cookie(request, PASSPORT_COOKIE);
        let token = match self
            .sessions(&binding, &adapter)
            .create(body.get("appSession"), &claims.sub, previous.as_deref())
            .await
        {
            Ok(token) => token,
            Err(error) => return fail(error),
        };
        let location = if state.ends_with(".admin") {
            "/admin"
        } else {
            "/chat"
        };
        let mut set_cookies = vec![session_cookie(&token)];
        set_cookies.extend(transient_cookies("", "", 0));
        Ok(AuthResponse {
            status: 302,
            headers: BTreeMap::from([
                ("location".into(), location.into()),
                ("cache-control".into(), "no-store".into()),
            ]),
            body: AuthResponseBody::Empty,
            set_cookies,
            stream: None,
        })
    }

    /// Local authority is deleted before best-effort upstream revocation; a
    /// local store failure never claims a logout.
    pub async fn logout(&self, request: &AuthRequest) -> Result<AuthResponse, GatewayError> {
        if !crate::origin::same_origin_mutation(request) {
            return Err(GatewayError::new(403, "Forbidden", Some("forbidden")));
        }
        let (binding, adapter) = (self.config.binding(), SessionAdapter(self.host));
        let token = cookie(request, PASSPORT_COOKIE).unwrap_or_default();
        let revoke = self.sessions(&binding, &adapter).revoke(&token).await?;
        Ok(AuthResponse {
            status: 204,
            headers: BTreeMap::from([
                ("cache-control".into(), "no-store".into()),
                (
                    "x-uos-passport-upstream-revoke".into(),
                    revoke.as_str().into(),
                ),
            ]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![clear_session_cookie()],
            stream: None,
        })
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
