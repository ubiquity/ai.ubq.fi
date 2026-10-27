//! Fixed production browser binding, PKCE and durable app-session ceremonies.
use gateway_auth_contracts::{
    types::{AuthHost, AuthRequest, AuthResponse, AuthResponseBody},
    upstream::AuthUpstreamRequest,
};
use gateway_auth_passport_contracts::{
    passport::{PASSPORT_AUTHORIZE_URL, PASSPORT_COOKIE, PASSPORT_STATE_COOKIE},
    session::{PASSPORT_COOKIE_MAX_AGE_SECONDS, PASSPORT_VERIFIER_COOKIE, PassportDate},
};
use gateway_auth_passport_sessions::sessions::{PassportSessions, token_shape};
use gateway_auth_primitives::{
    crypto::{Secret, base64url, sha256_base64url},
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
fn host_cookie(name: &str, value: &str, max_age: i64) -> String {
    format!("{name}={value}; Max-Age={max_age}; Path=/; HttpOnly; Secure; SameSite=Lax")
}
pub fn session_cookie(token: &str) -> String {
    host_cookie(PASSPORT_COOKIE, token, PASSPORT_COOKIE_MAX_AGE_SECONDS)
}
pub fn clear_session_cookie() -> String {
    host_cookie(PASSPORT_COOKIE, "", 0)
}
fn transient_clear() -> Vec<String> {
    vec![
        host_cookie(PASSPORT_STATE_COOKIE, "", 0),
        host_cookie(PASSPORT_VERIFIER_COOKIE, "", 0),
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
    let clear = error.status == 401
        && cookie(request, PASSPORT_COOKIE).is_some_and(|token| token_shape(&token));
    let mut response = if cookie(request, PASSPORT_COOKIE).is_some() {
        error_response(error)
    } else {
        AuthResponse::error(error)
    };
    if clear {
        response.set_cookies.push(clear_session_cookie());
    }
    response
}
/// Call only after the existing report has successfully authenticated.
pub fn renew_report_cookie(request: &AuthRequest, response: &mut AuthResponse) {
    if (200..300).contains(&response.status)
        && let Some(token) = cookie(request, PASSPORT_COOKIE).filter(|token| token_shape(token))
    {
        response.set_cookies.push(session_cookie(&token));
    }
}
pub struct PassportBrowser<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
}
impl<S: gateway_core::state::AuthorityStore, H: AuthHost + PassportDate> PassportBrowser<'_, S, H> {
    pub async fn route(&self, request: &AuthRequest) -> Option<AuthResponse> {
        let url = Url::parse(&request.url).ok()?;
        let path = url.path();
        if !matches!(path, "/auth/login" | "/auth/callback" | "/auth/logout") {
            return None;
        }
        let result = if !production_request(request) {
            Err(unauthorized())
        } else if request.method
            != if path == "/auth/logout" {
                "POST"
            } else {
                "GET"
            }
        {
            Err(GatewayError::new(
                405,
                "Method not allowed",
                Some("method_not_allowed"),
            ))
        } else {
            match path {
                "/auth/login" => self.login(&url),
                "/auth/callback" => {
                    return Some(match self.callback(request, &url).await {
                        Ok(response) => response,
                        Err(error) => {
                            let mut response = error_response(error);
                            response.set_cookies = transient_clear();
                            response
                        }
                    });
                }
                _ => self.logout(request).await,
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
    fn login(&self, url: &Url) -> Result<AuthResponse, GatewayError> {
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
            .append_pair("client_id", "ai")
            .append_pair("audience", "ai")
            .append_pair("origin", "https://ai.ubq.fi")
            .append_pair("redirect_uri", "https://ai.ubq.fi/auth/callback")
            .append_pair("state", &state)
            .append_pair("code_challenge", &sha256_base64url(verifier.as_bytes()))
            .append_pair("code_challenge_method", "S256");
        Ok(redirect(
            authorize.to_string(),
            vec![
                host_cookie(PASSPORT_STATE_COOKIE, &state, 600),
                host_cookie(PASSPORT_VERIFIER_COOKIE, &verifier, 600),
            ],
        ))
    }
    async fn callback(
        &self,
        request: &AuthRequest,
        url: &Url,
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
        let verifier = cookie(request, PASSPORT_VERIFIER_COOKIE)
            .filter(|value| suffix(value))
            .ok_or_else(unauthorized)?;
        if !suffix(nonce)
            || !matches!(target, "admin" | "chat")
            || cookie(request, PASSPORT_STATE_COOKIE).as_ref() != Some(state)
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
        let claims = crate::passport::verify_source(self.host, &token).await?;
        if claims.agent_token_id.is_some() {
            return Err(unauthorized());
        }
        let grant = gateway_source::source_serialization::member(&document, "appSession")
            .ok_or_else(gateway_auth_passport_sessions::sessions::unavailable)?;
        let local = PassportSessions {
            store: self.store,
            host: self.host,
        }
        .create(
            &grant,
            &claims.sub,
            cookie(request, PASSPORT_COOKIE).as_deref(),
        )
        .await?;
        let mut cookies = vec![session_cookie(&local)];
        cookies.extend(transient_clear());
        Ok(redirect(format!("/{target}"), cookies))
    }
    async fn logout(&self, request: &AuthRequest) -> Result<AuthResponse, GatewayError> {
        if request
            .header("origin")
            .is_some_and(|origin| origin != "https://ai.ubq.fi")
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
        .revoke(&cookie(request, PASSPORT_COOKIE).unwrap_or_default())
        .await?;
        Ok(AuthResponse {
            status: 204,
            headers: BTreeMap::from([
                ("cache-control".into(), "no-store".into()),
                ("x-uos-passport-upstream-revoke".into(), result.into()),
            ]),
            body: AuthResponseBody::Empty,
            set_cookies: vec![clear_session_cookie()],
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
