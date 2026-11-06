//! Persistent Passport sign-in: the admin return path, PKCE exchange and the
//! revocable app session (source tests/passport.test.ts, passport-sessions.test.ts).
use gateway_auth::{
    passport::{
        PASSPORT_COOKIE, PASSPORT_STATE_COOKIE, PASSPORT_VERIFIER_COOKIE, PassportConfig,
        PassportService, reset_jwks_cache,
    },
    resolve::AuthService,
    upstream::AuthUpstreamRequest,
};
use serde_json::{Value, json};

mod passport_support;
use passport_support::*;

#[test]
fn admin_button_destination_exchanges_with_pkce_and_stores_a_persistent_session() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let config = config();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let callback = begin(&passport, "/admin")?;
    let verifier = gateway_auth::passport::cookie(&callback, PASSPORT_VERIFIER_COOKIE).must()?;
    let response = run(passport.callback(&callback)).must()?;
    assert_eq!(response.status, 302);
    assert_eq!(response.headers["location"], "/admin");
    assert_eq!(response.set_cookies.len(), 3);
    let session = cookie_value(&response.set_cookies[0], PASSPORT_COOKIE)?;
    assert!(session.starts_with("uos_ai_passport_session_"));
    assert_eq!(
        response.set_cookies[0],
        format!(
            "{PASSPORT_COOKIE}={session}; Path=/; Max-Age=34560000; HttpOnly; Secure; SameSite=Lax"
        )
    );
    assert!(
        response.set_cookies[1]
            .starts_with(&format!("{PASSPORT_STATE_COOKIE}=; Path=/; Max-Age=0;"))
    );
    assert!(
        response.set_cookies[2]
            .starts_with(&format!("{PASSPORT_VERIFIER_COOKIE}=; Path=/; Max-Age=0;"))
    );
    assert_eq!(store.rows.borrow().len(), 1);
    let exchange = host.exchange_requests.borrow();
    assert_eq!(exchange.len(), 1);
    let AuthUpstreamRequest::PassportExchange {
        code,
        code_verifier,
        ..
    } = &exchange[0]
    else {
        panic!("expected Passport exchange");
    };
    assert_eq!(code.expose(), "code-1");
    assert_eq!(code_verifier.expose(), verifier);
    let body: Value = serde_json::from_slice(&exchange[0].body().must()?).must()?;
    assert_eq!(
        body,
        bound(json!({"code":"code-1","codeVerifier":verifier,"appSession":true}))
    );
    let keys = body.as_object().must()?.keys().cloned().collect::<Vec<_>>();
    assert_eq!(
        keys,
        [
            "code",
            "clientId",
            "audience",
            "origin",
            "redirectUri",
            "codeVerifier",
            "appSession"
        ]
    );
    drop(exchange);
    // The stored session authenticates the auth report, admin reads and clients.
    let auth = AuthService::new(&store, &host, &config);
    let report = with_cookie(
        request("GET", "https://ai.ubq.fi/uos/auth"),
        PASSPORT_COOKIE,
        &session,
    );
    let response = run(auth.handle_auth_route(&report)).must()?.must()?;
    let payload: Value = serde_json::from_slice(&response.body.to_bytes().must()?).must()?;
    assert_eq!(payload["auth"]["is_admin"], true);
    assert_eq!(payload["auth"]["principal"], "passport-user:user-123");
    assert_eq!(payload["auth"]["method"]["kind"], "passport_session");
    assert_eq!(payload["auth"]["method"]["user"]["exp"], Value::Null);
    assert_eq!(payload["auth"]["method"]["user"]["jti"], Value::Null);
    assert_eq!(payload["auth"]["method"]["user"]["auth"], "sso");
    assert_eq!(payload["auth"]["token"]["present"], false);
    assert_eq!(
        response.set_cookies,
        [gateway_auth::passport::session_cookie(&session)]
    );
    let admin = with_cookie(
        request("GET", "https://ai.ubq.fi/admin/api-keys"),
        PASSPORT_COOKIE,
        &session,
    );
    assert!(run(auth.authenticate_admin(&admin)).must()?.is_admin);
    let client = run(auth.authenticate_client(&admin)).must()?;
    assert_eq!(client.method.principal_id(None), "passport-user:user-123");
    assert_eq!(host.session_calls.borrow().len(), 3);
    Ok(())
}

#[test]
fn callback_only_returns_to_fixed_local_destinations() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    for target in ["https://evil.example/admin", "/admin/", "/chat", ""] {
        let response = run(passport.callback(&begin(&passport, target)?)).must()?;
        assert_eq!(response.headers["location"], "/chat", "{target}");
    }
    Ok(())
}

#[test]
fn failed_callback_clears_only_transient_cookies_and_never_admits_an_agent() -> TestResult {
    reset_jwks_cache();
    let mut agent = claims();
    agent["agentTokenId"] = json!("agent-id");
    let host = Host::new(vec![public_jwk("key-a")?], token(&agent)?)?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let mut callback = begin(&passport, "/admin")?;
    let cookies = format!(
        "{}; {PASSPORT_COOKIE}=existing-session",
        callback.headers["cookie"]
    );
    callback.headers.insert("cookie".into(), cookies);
    let response = run(passport.callback(&callback)).must()?;
    assert_eq!(response.status, 401);
    let names = response
        .set_cookies
        .iter()
        .map(|value| value.split('=').next().must())
        .collect::<TestResult<Vec<_>>>()?;
    assert_eq!(names, [PASSPORT_STATE_COOKIE, PASSPORT_VERIFIER_COOKIE]);
    assert_eq!(store.rows.borrow().len(), 0);
    // An exchange without a valid app grant cannot create a session either.
    *host.exchange.borrow_mut() =
        serde_json::to_vec(&json!({"accessToken":token(&claims())?})).must()?;
    let response = run(passport.callback(&begin(&passport, "/admin")?)).must()?;
    assert_eq!(response.status, 503);
    assert_eq!(store.rows.borrow().len(), 0);
    Ok(())
}

#[test]
fn callback_rejects_missing_mismatched_and_replayed_state_without_exchange() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let valid = begin(&passport, "/admin")?;
    let state = gateway_auth::passport::cookie(&valid, PASSPORT_STATE_COOKIE).must()?;
    let state_only = with_cookie(valid.clone(), PASSPORT_STATE_COOKIE, &state);
    let bad_verifier = format!(
        "{}; {PASSPORT_VERIFIER_COOKIE}=short",
        state_only.headers["cookie"]
    );
    let mut bad_verifier_request = valid.clone();
    bad_verifier_request
        .headers
        .insert("cookie".into(), bad_verifier);
    let unbound = state.replace(".admin", ".other");
    let mut unbound_request = valid.clone();
    unbound_request.url = valid.url.replace(&state, &unbound);
    let cookies = valid.headers["cookie"].replace(&state, &unbound);
    unbound_request.headers.insert("cookie".into(), cookies);
    for request in [
        state_only,
        bad_verifier_request,
        unbound_request,
        request("GET", "https://ai.ubq.fi/auth/callback?state=a&code=b"),
        with_cookie(
            request("GET", "https://ai.ubq.fi/auth/callback?state=a&code=b"),
            PASSPORT_STATE_COOKIE,
            "other",
        ),
        with_cookie(
            request(
                "GET",
                "https://ai.ubq.fi/auth/callback?state=a&state=b&code=c",
            ),
            PASSPORT_STATE_COOKIE,
            "a",
        ),
        with_cookie(
            request("GET", "https://ai.ubq.fi/auth/callback?state=a"),
            PASSPORT_STATE_COOKIE,
            "a",
        ),
    ] {
        let response = run(passport.callback(&request)).must()?;
        assert_eq!(response.status, 401);
        assert!(
            response
                .set_cookies
                .iter()
                .any(|cookie| cookie.starts_with(&format!("{PASSPORT_STATE_COOKIE}=;")))
        );
    }
    assert_eq!(host.exchange_requests.borrow().len(), 0);
    assert_eq!(host.jwks_fetches.get(), 0);
    Ok(())
}

/// Completes a real admin sign-in and returns the stored session cookie value.
fn signed_in(store: &Store, host: &Host) -> TestResult<String> {
    let passport = PassportService {
        store,
        host,
        config: &PassportConfig::production(),
    };
    let response = run(passport.callback(&begin(&passport, "/admin")?)).must()?;
    cookie_value(&response.set_cookies[0], PASSPORT_COOKIE)
}

#[test]
fn persistent_admin_authority_follows_current_passport_role_and_availability() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let (store, config) = (Store::default(), config());
    let session = signed_in(&store, &host)?;
    let auth = AuthService::new(&store, &host, &config);
    let admin = with_cookie(
        request("GET", "https://ai.ubq.fi/admin"),
        PASSPORT_COOKIE,
        &session,
    );
    // Elapsed time beyond any access-token lifetime never expires the session.
    host.now.set(i64::from(i32::MAX) * 1000);
    assert!(run(auth.authenticate_admin(&admin)).is_ok());
    host.session_admin.set(false);
    assert_eq!(
        run(auth.authenticate_admin(&admin)).err().must()?.status,
        403
    );
    host.session_status.set(503);
    assert_eq!(
        run(auth.authenticate_admin(&admin)).err().must()?.status,
        503
    );
    assert_eq!(
        run(auth.authenticate_client(&admin)).err().must()?.status,
        503
    );
    assert_eq!(store.rows.borrow().len(), 1);
    host.session_status.set(200);
    host.session_admin.set(true);
    let mut foreign = with_cookie(
        request("POST", "https://ai.ubq.fi/admin/api"),
        PASSPORT_COOKIE,
        &session,
    );
    foreign
        .headers
        .insert("origin".into(), "https://evil.example".into());
    assert_eq!(
        run(auth.authenticate_admin(&foreign)).err().must()?.status,
        403
    );
    assert!(run(auth.authenticate_admin(&admin)).is_ok());
    Ok(())
}

#[test]
fn authoritative_revocation_removes_authority_and_clears_the_browser_cookie() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let (store, config) = (Store::default(), config());
    let session = signed_in(&store, &host)?;
    let auth = AuthService::new(&store, &host, &config);
    host.session_status.set(401);
    let report = with_cookie(
        request("GET", "https://ai.ubq.fi/uos/auth"),
        PASSPORT_COOKIE,
        &session,
    );
    let response = run(auth.handle_auth_route(&report)).must()?.must()?;
    assert_eq!(response.status, 401);
    assert_eq!(
        response.set_cookies,
        [gateway_auth::passport::clear_session_cookie()]
    );
    assert!(
        response.set_cookies[0].starts_with(&format!("{PASSPORT_COOKIE}=; Path=/; Max-Age=0;"))
    );
    assert_eq!(store.rows.borrow().len(), 0);
    host.session_status.set(200);
    assert_eq!(
        run(auth.authenticate_admin(&report)).err().must()?.status,
        401
    );
    assert_eq!(host.session_calls.borrow().len(), 1);
    // A legacy JWT cookie failure is not a revocation and clears nothing.
    let legacy = with_cookie(report.clone(), PASSPORT_COOKIE, "a.b.c");
    assert!(run(auth.handle_auth_route(&legacy)).is_err());
    Ok(())
}

#[test]
fn logout_revokes_local_authority_first_and_rejects_foreign_origins() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let (store, config) = (Store::default(), config());
    let session = signed_in(&store, &host)?;
    let auth = AuthService::new(&store, &host, &config);
    let logout = with_cookie(
        request("POST", "https://ai.ubq.fi/auth/logout"),
        PASSPORT_COOKIE,
        &session,
    );
    let mut foreign = logout.clone();
    foreign
        .headers
        .insert("origin".into(), "https://evil.example".into());
    assert_eq!(
        run(auth.handle_auth_route(&foreign)).err().must()?.status,
        403
    );
    assert_eq!(store.rows.borrow().len(), 1);
    let mut same = logout.clone();
    same.headers
        .insert("origin".into(), "https://ai.ubq.fi".into());
    host.session_status.set(503);
    let response = run(auth.handle_auth_route(&same)).must()?.must()?;
    assert_eq!(response.status, 204);
    assert_eq!(
        response.headers["x-uos-passport-upstream-revoke"],
        "unavailable"
    );
    assert_eq!(
        response.set_cookies,
        [format!(
            "{PASSPORT_COOKIE}=; Path=/; Max-Age=0; HttpOnly; Secure; SameSite=Lax"
        )]
    );
    assert_eq!(store.rows.borrow().len(), 0);
    assert_eq!(
        host.session_calls.borrow().as_slice(),
        ["https://passport.ubq.fi/api/auth/sso/session/revoke"]
    );
    let response = run(auth.handle_auth_route(&same)).must()?.must()?;
    assert_eq!(
        response.headers["x-uos-passport-upstream-revoke"],
        "not_applicable"
    );
    Ok(())
}

#[test]
fn a_new_sign_in_atomically_replaces_the_previous_local_session() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let previous = signed_in(&store, &host)?;
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let mut callback = begin(&passport, "/chat")?;
    let cookies = format!(
        "{}; {PASSPORT_COOKIE}={previous}",
        callback.headers["cookie"]
    );
    callback.headers.insert("cookie".into(), cookies);
    let response = run(passport.callback(&callback)).must()?;
    assert_eq!(response.headers["location"], "/chat");
    assert_ne!(
        cookie_value(&response.set_cookies[0], PASSPORT_COOKIE)?,
        previous
    );
    assert_eq!(store.rows.borrow().len(), 1);
    let duplicate = format!("{PASSPORT_COOKIE}={previous}; {PASSPORT_COOKIE}={previous}");
    let mut ambiguous = request("GET", "https://ai.ubq.fi/admin");
    ambiguous.headers.insert("cookie".into(), duplicate);
    assert_eq!(
        gateway_auth::passport::cookie(&ambiguous, PASSPORT_COOKIE),
        None
    );
    Ok(())
}
