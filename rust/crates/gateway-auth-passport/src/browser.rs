//! Fixed production browser binding, PKCE and durable app-session ceremonies.
use gateway_auth_contracts::{
    types::{AuthConfig, AuthHost, AuthRequest, AuthResponse, AuthResponseBody},
    upstream::AuthUpstreamRequest,
};
use gateway_auth_passport_contracts::{
    passport::{PASSPORT_AUTHORIZE_URL, PASSPORT_COOKIE, PASSPORT_STATE_COOKIE, PassportConfig},
    session::{PASSPORT_COOKIE_MAX_AGE_SECONDS, PASSPORT_VERIFIER_COOKIE, PassportDate},
};
use gateway_auth_passport_sessions::sessions::{PassportSessions, token_shape};
use gateway_auth_primitives::{
    crypto::{Secret, base64url, sha256_base64url, sha256_hex},
    error::unauthorized,
};
use gateway_source::error::GatewayError;
use std::collections::BTreeMap;
use url::Url;

pub fn production_request(request: &AuthRequest) -> bool {
    Url::parse(&request.url).is_ok_and(|url| {
        url.host_str() == Some("ai.ubq.fi")
            && url.port().is_none()
            && (url.scheme() == "https"
                || url.scheme() == "http" && request.header("x-forwarded-proto") == Some("https"))
    })
}
/// Duplicate cookie names are ambiguous, even when their values match.
pub fn cookie(request: &AuthRequest, name: &str) -> Option<String> {
    let prefix = format!("{name}=");
    let mut values = request
        .header("cookie")?
        .split(';')
        .map(str::trim)
        .filter_map(|part| part.strip_prefix(&prefix));
    let value = values.next()?;
    if values.next().is_some() {
        return None;
    }
    Some(value.into())
}
/// Names are DATA from the validated deployment binding, never from caller headers.
pub fn cookie_names(binding: &PassportConfig) -> [String; 3] {
    if !binding.http_loopback() {
        return [
            PASSPORT_COOKIE.into(),
            PASSPORT_STATE_COOKIE.into(),
            PASSPORT_VERIFIER_COOKIE.into(),
        ];
    }
    let tuple = serde_json::json!([
        "uos-ai-passport-cookie-v1",
        binding.client_id,
        binding.audience,
        binding.origin,
        binding.redirect_uri
    ])
    .to_string();
    let digest = sha256_hex(tuple.as_bytes());
    ["session", "state", "verifier"].map(|purpose| format!("uos_ai_passport_{digest}_{purpose}"))
}
pub fn binding_for_request(config: &AuthConfig, request: &AuthRequest) -> Option<PassportConfig> {
    let binding = crate::passport::config_for_request(config, request).or_else(|| {
        (production_request(request)
            && config
                .passport
                .as_ref()
                .is_none_or(|value| *value == PassportConfig::production()))
        .then(PassportConfig::production)
    })?;
    let url = Url::parse(&request.url).ok()?;
    ((binding == PassportConfig::production() && production_request(request))
        || url.origin().ascii_serialization() == binding.origin
            && url.username().is_empty()
            && url.password().is_none())
    .then_some(binding)
}
fn bound_cookie(binding: &PassportConfig, index: usize, value: &str, max_age: i64) -> String {
    let secure = if binding.http_loopback() {
        ""
    } else {
        "; Secure"
    };
    format!(
        "{}={value}; Max-Age={max_age}; Path=/; HttpOnly{secure}; SameSite=Lax",
        cookie_names(binding)[index]
    )
}
fn host_cookie(name: &str, value: &str, max_age: i64) -> String {
    format!("{name}={value}; Max-Age={max_age}; Path=/; HttpOnly; Secure; SameSite=Lax")
}
pub fn session_cookie(token: &str) -> String {
    host_cookie(PASSPORT_COOKIE, token, PASSPORT_COOKIE_MAX_AGE_SECONDS)
}
pub fn clear_session_cookie() -> String {
    host_cookie(PASSPORT_COOKIE, "", 0)
}
fn transient_clear(binding: &PassportConfig) -> Vec<String> {
    vec![
        bound_cookie(binding, 1, "", 0),
        bound_cookie(binding, 2, "", 0),
    ]
}
fn redirect(location: String, cookies: Vec<String>) -> AuthResponse {
    AuthResponse {
        status: 302,
        headers: BTreeMap::from([
            ("location".into(), location),
            ("cache-control".into(), "no-store".into()),
            ("referrer-policy".into(), "no-referrer".into()),
        ]),
        body: AuthResponseBody::Empty,
        set_cookies: cookies,
        stream: None,
    }
}
fn suffix(value: &str) -> bool {
    value.len() == 43
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-'))
}
fn error_response(error: GatewayError) -> AuthResponse {
    // Frozen passport.ts::errorResponse, restricted to this Passport boundary.
    let error = match error.status {
        401 => unauthorized(),
        502 | 503 => GatewayError::new(
            error.status,
            "Passport verification is unavailable.",
            Some("passport_unavailable"),
        ),
        _ => error,
    };
    let mut response = AuthResponse::error(error);
    response
        .headers
        .insert("referrer-policy".into(), "no-referrer".into());
    response
}
pub fn authentication_error(request: &AuthRequest, error: GatewayError) -> AuthResponse {
    authentication_error_bound(request, error, &PassportConfig::production())
}
pub fn authentication_error_bound(
    request: &AuthRequest,
    error: GatewayError,
    binding: &PassportConfig,
) -> AuthResponse {
    let name = &cookie_names(binding)[0];
    let clear =
        error.status == 401 && cookie(request, name).is_some_and(|token| token_shape(&token));
    let mut response = if cookie(request, name).is_some() {
        error_response(error)
    } else {
        AuthResponse::error(error)
    };
    if clear {
        response.set_cookies.push(bound_cookie(binding, 0, "", 0));
    }
    response
}
/// Call only after the existing report has successfully authenticated.
pub fn renew_report_cookie(request: &AuthRequest, response: &mut AuthResponse) {
    renew_report_cookie_bound(request, response, &PassportConfig::production());
}
pub fn renew_report_cookie_bound(
    request: &AuthRequest,
    response: &mut AuthResponse,
    binding: &PassportConfig,
) {
    if (200..300).contains(&response.status)
        && let Some(token) =
            cookie(request, &cookie_names(binding)[0]).filter(|token| token_shape(token))
    {
        response.set_cookies.push(bound_cookie(
            binding,
            0,
            &token,
            PASSPORT_COOKIE_MAX_AGE_SECONDS,
        ));
    }
}
pub struct PassportBrowser<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
}
impl<S: gateway_core::state::AuthorityStore, H: AuthHost + PassportDate> PassportBrowser<'_, S, H> {
    pub async fn route(&self, request: &AuthRequest) -> Option<AuthResponse> {
        self.route_for(
            request,
            &AuthConfig {
                passport: Some(PassportConfig::production()),
                ..Default::default()
            },
        )
        .await
    }
    pub async fn route_for(
        &self,
        request: &AuthRequest,
        config: &AuthConfig,
    ) -> Option<AuthResponse> {
        let url = Url::parse(&request.url).ok()?;
        let path = url.path();
        if !matches!(path, "/auth/login" | "/auth/callback" | "/auth/logout") {
            return None;
        }
        let Some(binding) = binding_for_request(config, request) else {
            return Some(error_response(unauthorized()));
        };
        let result = if request.method
            != if path == "/auth/logout" {
                "POST"
            } else {
                "GET"
            } {
            Err(GatewayError::new(
                405,
                "Method not allowed",
                Some("method_not_allowed"),
            ))
        } else {
            match path {
                "/auth/login" => self.login(&url, &binding),
                "/auth/callback" => {
                    return Some(match self.callback(request, &url, &binding).await {
                        Ok(response) => response,
                        Err(error) => {
                            let mut response = error_response(error);
                            response.set_cookies = transient_clear(&binding);
                            response
                        }
                    });
                }
                _ => self.logout(request, &binding).await,
            }
        };
        Some(result.unwrap_or_else(error_response))
    }
    fn random(&self) -> Result<String, GatewayError> {
        let bytes = self.host.random_bytes(32)?;
        if bytes.len() != 32 {
            return Err(gateway_auth_passport_sessions::sessions::unavailable());
        }
        Ok(base64url(&bytes))
    }
    fn login(&self, url: &Url, binding: &PassportConfig) -> Result<AuthResponse, GatewayError> {
        let target = if url
            .query_pairs()
            .find(|(key, _)| key == "return_to")
            .is_some_and(|(_, value)| value == "/admin")
        {
            "admin"
        } else {
            "chat"
        };
        let state = format!("{}.{target}", self.random()?);
        let verifier = self.random()?;
        let mut authorize = Url::parse(PASSPORT_AUTHORIZE_URL).map_err(|_| unauthorized())?;
        authorize
            .query_pairs_mut()
            .append_pair("client_id", &binding.client_id)
            .append_pair("audience", &binding.audience)
            .append_pair("origin", &binding.origin)
            .append_pair("redirect_uri", &binding.redirect_uri)
            .append_pair("state", &state)
            .append_pair("code_challenge", &sha256_base64url(verifier.as_bytes()))
            .append_pair("code_challenge_method", "S256");
        if binding.http_loopback() {
            authorize
                .query_pairs_mut()
                .append_pair("prompt", "select_account");
        }
        Ok(redirect(
            authorize.to_string(),
            vec![
                bound_cookie(binding, 1, &state, 600),
                bound_cookie(binding, 2, &verifier, 600),
            ],
        ))
    }
    async fn callback(
        &self,
        request: &AuthRequest,
        url: &Url,
        binding: &PassportConfig,
    ) -> Result<AuthResponse, GatewayError> {
        let states = url
            .query_pairs()
            .filter(|(key, _)| key == "state")
            .map(|(_, value)| value.into_owned())
            .collect::<Vec<_>>();
        let codes = url
            .query_pairs()
            .filter(|(key, _)| key == "code")
            .map(|(_, value)| value.into_owned())
            .collect::<Vec<_>>();
        let ([state], [code]) = (states.as_slice(), codes.as_slice()) else {
            return Err(unauthorized());
        };
        let Some((nonce, target)) = state.split_once('.') else {
            return Err(unauthorized());
        };
        let verifier = cookie(request, &cookie_names(binding)[2])
            .filter(|value| suffix(value))
            .ok_or_else(unauthorized)?;
        if !suffix(nonce)
            || !matches!(target, "admin" | "chat")
            || cookie(request, &cookie_names(binding)[1]).as_ref() != Some(state)
            || code.is_empty()
            || code.encode_utf16().count() > 500
        {
            return Err(unauthorized());
        }
        let response = self
            .host
            .fetch_auth(AuthUpstreamRequest::PassportPkceExchange {
                code: Secret::new(code.clone()),
                code_verifier: Secret::new(verifier),
                binding: binding.clone(),
            })
            .await
            .map_err(|error| match error.envelope.error.code.as_deref() {
                Some("upstream_response_too_large" | "passport_exchange_body_missing") => {
                    exchange_unavailable()
                }
                _ => unauthorized(),
            })?;
        if !response.success() || response.body.len() > 65_536 {
            return Err(exchange_unavailable());
        }
        let document = gateway_auth_passport_contracts::session::parse_source_json(&response.body)
            .map_err(|_| unauthorized())?;
        let token = gateway_source::source_serialization::member(&document, "accessToken")
            .and_then(|v| v.deserialize::<String>().ok())
            .ok_or_else(unauthorized)?;
        let claims = crate::passport::verify_source_for_config(self.host, binding, &token).await?;
        if claims.agent_token_id.is_some() {
            return Err(unauthorized());
        }
        let grant = gateway_source::source_serialization::member(&document, "appSession")
            .ok_or_else(gateway_auth_passport_sessions::sessions::unavailable)?;
        let local = PassportSessions {
            store: self.store,
            host: self.host,
        }
        .create_for(
            &grant,
            &claims.sub,
            cookie(request, &cookie_names(binding)[0]).as_deref(),
            binding,
        )
        .await?;
        let mut cookies = vec![bound_cookie(
            binding,
            0,
            &local,
            PASSPORT_COOKIE_MAX_AGE_SECONDS,
        )];
        cookies.extend(transient_clear(binding));
        Ok(redirect(format!("/{target}"), cookies))
    }
    async fn logout(
        &self,
        request: &AuthRequest,
        binding: &PassportConfig,
    ) -> Result<AuthResponse, GatewayError> {
        if request
            .header("origin")
            .is_some_and(|origin| origin != binding.origin)
            || request
                .header("sec-fetch-site")
                .is_some_and(|site| !matches!(site, "same-origin" | "none"))
        {
            return Err(GatewayError::new(403, "Forbidden", Some("forbidden")));
        }
        let result = PassportSessions {
            store: self.store,
            host: self.host,
        }
        .revoke_for(
            &cookie(request, &cookie_names(binding)[0]).unwrap_or_default(),
            binding,
        )
        .await?;
        Ok(AuthResponse {
            status: 204,
            headers: BTreeMap::from([
                ("cache-control".into(), "no-store".into()),
                ("x-uos-passport-upstream-revoke".into(), result.into()),
            ]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![bound_cookie(binding, 0, "", 0)],
            stream: None,
        })
    }
}
fn exchange_unavailable() -> GatewayError {
    GatewayError::new(
        502,
        "Passport verification is unavailable.",
        Some("passport_unavailable"),
    )
}
