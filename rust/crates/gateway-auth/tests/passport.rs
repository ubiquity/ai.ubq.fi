use gateway_auth::{
    crypto::base64url,
    origin::same_origin_mutation,
    passport::{
        PASSPORT_COOKIE, PASSPORT_STATE_COOKIE, PASSPORT_VERIFIER_COOKIE, PassportClaims,
        PassportConfig, PassportService, jwt_shaped, reset_jwks_cache, verify_jwt,
    },
    resolve::AuthService,
    types::{AuthConfig, AuthMethod},
};
use gateway_core::error::GatewayError;
use serde_json::{Value, json};
use std::collections::BTreeMap;

mod passport_support;
use passport_support::*;

#[test]
fn login_redirect_contains_the_exact_fixed_authorize_contract() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let response = run(passport.login(&request("GET", "https://ai.ubq.fi/auth/login"))).must()?;
    assert_eq!(response.status, 302);
    let location = url::Url::parse(&response.headers["location"]).must()?;
    assert_eq!(
        location.origin().ascii_serialization(),
        "https://passport.ubq.fi"
    );
    assert_eq!(location.path(), "/authorize");
    let query = location.query_pairs().collect::<BTreeMap<_, _>>();
    assert_eq!(query["client_id"], "ai");
    assert_eq!(query["audience"], "ai");
    assert_eq!(query["origin"], "https://ai.ubq.fi");
    assert_eq!(query["redirect_uri"], "https://ai.ubq.fi/auth/callback");
    assert_eq!(query["code_challenge_method"], "S256");
    assert_eq!(response.set_cookies.len(), 2);
    let state = query["state"].to_string();
    assert!(state.ends_with(".chat") && state.len() == 48);
    assert_eq!(
        response.set_cookies[0],
        format!(
            "{PASSPORT_STATE_COOKIE}={state}; Path=/; Max-Age=600; HttpOnly; Secure; SameSite=Lax"
        )
    );
    let verifier = cookie_value(&response.set_cookies[1], PASSPORT_VERIFIER_COOKIE)?;
    assert!(response.set_cookies[1].ends_with("; Max-Age=600; HttpOnly; Secure; SameSite=Lax"));
    assert_eq!(verifier.len(), 43);
    assert_eq!(
        query["code_challenge"],
        gateway_passport_session::binding::pkce_challenge(&verifier)
    );
    Ok(())
}

#[test]
fn callback_rejects_exchange_status_and_malformed_token() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], "not-a-jwt".into())?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    let request = begin(&passport, "/admin")?;
    assert_eq!(run(passport.callback(&request)).must()?.status, 401);
    host.exchange_status.set(400);
    assert_eq!(run(passport.callback(&request)).must()?.status, 401);
    Ok(())
}

#[test]
fn claim_signature_and_algorithm_failures_fail_closed() -> TestResult {
    reset_jwks_cache();
    let config = PassportConfig::production();
    let public = private()?.to_public_key();
    let keys = BTreeMap::from([("key-a".to_owned(), public)]);
    let valid = token(&claims())?;
    assert!(verify_jwt(&valid, &config, 1_500, &keys).is_ok());

    let mut cases = Vec::new();
    let mutate = |name: &str, value: Value| {
        let mut claims = claims();
        claims[name] = value;
        claims
    };
    cases.push(("issuer", mutate("iss", json!("https://evil.example"))));
    cases.push(("audience", mutate("aud", json!("other"))));
    cases.push(("client", mutate("clientId", json!("other"))));
    cases.push(("expired", mutate("exp", json!(1_499))));
    cases.push(("future-iat", mutate("iat", json!(1_561))));
    cases.push(("empty-jti", mutate("jti", json!(""))));
    cases.push(("exp-before-iat", mutate("exp", json!(999))));
    let mut missing = claims();
    missing.as_object_mut().must()?.remove("jti");
    cases.push(("missing-jti", missing));
    for (name, claims) in cases {
        let token = token(&claims)?;
        assert!(
            verify_jwt(&token, &config, 1_500, &keys).is_err(),
            "{name} must fail"
        );
    }
    for header in [
        json!({"alg":"none","typ":"JWT","kid":"key-a"}),
        json!({"alg":"HS256","typ":"JWT","kid":"key-a"}),
        json!({"alg":"ES256","typ":"JWT","kid":"key-a"}),
        json!({"alg":"RS256","typ":"JWT","kid":"other"}),
        json!({"alg":"RS256","typ":"JWT","kid":"key-a","jku":"https://evil.example"}),
        json!({"alg":"RS256","typ":"JWT","kid":"key-a","x5u":"https://evil.example"}),
        json!({"alg":"RS256","typ":"JWT","kid":"key-a","jwk":{"kty":"RSA"}}),
        json!({"alg":"RS256","typ":"JWT"}),
        json!({"alg":"RS256","kid":"key-a"}),
    ] {
        assert!(verify_jwt(&jwt(header, &claims())?, &config, 1_500, &keys).is_err());
    }
    let tampered = valid
        .rsplit_once('.')
        .map(|(prefix, _)| format!("{prefix}.AAAA"))
        .must()?;
    assert!(verify_jwt(&tampered, &config, 1_500, &keys).is_err());
    Ok(())
}

#[test]
fn jwks_cache_refreshes_exactly_once_for_an_unknown_kid() -> TestResult {
    reset_jwks_cache();
    let token_a = token(&claims())?;
    let mut token_b_claims = claims();
    token_b_claims["jti"] = json!("jti-2");
    token_b_claims["sub"] = json!("user-456");
    let token_b = jwt(
        json!({"alg":"RS256","typ":"JWT","kid":"key-b"}),
        &token_b_claims,
    )?;
    let host = Host::new(vec![public_jwk("key-a")?], token_a.clone())?;
    let store = Store::default();
    let passport = PassportService {
        store: &store,
        host: &host,
        config: &PassportConfig::production(),
    };
    assert_eq!(run(passport.bearer(&token_a)).must()?.sub, "user-123");
    assert_eq!(host.jwks_fetches.get(), 1);
    assert!(run(passport.bearer(&token_a)).is_ok());
    assert_eq!(host.jwks_fetches.get(), 1);
    // Rotation publishes the new key; one refresh serves key-b.
    *host.jwks.borrow_mut() =
        serde_json::to_vec(&json!({"keys":[public_jwk("key-a")?,public_jwk("key-b")?]})).must()?;
    assert!(run(passport.bearer(&token_b)).is_ok());
    assert_eq!(host.jwks_fetches.get(), 2);
    let unknown = jwt(json!({"alg":"RS256","typ":"JWT","kid":"key-c"}), &claims())?;
    assert!(run(passport.bearer(&unknown)).is_err());
    assert_eq!(host.jwks_fetches.get(), 3);
    Ok(())
}

#[test]
fn jwks_transport_failure_and_non_success_status_are_unavailable() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    *host.jwks.borrow_mut() = b"not json".to_vec();
    assert!(
        run(PassportService {
            store: &Store::default(),
            host: &host,
            config: &PassportConfig::production()
        }
        .bearer(&token(&claims())?))
        .is_err()
    );
    reset_jwks_cache();
    host.jwks_status.set(302);
    *host.jwks.borrow_mut() = serde_json::to_vec(&json!({"keys":[public_jwk("key-a")?]})).must()?;
    assert!(
        run(PassportService {
            store: &Store::default(),
            host: &host,
            config: &PassportConfig::production()
        }
        .bearer(&token(&claims())?))
        .is_err()
    );
    Ok(())
}

fn assert_unauthorized(result: Result<gateway_auth::types::AuthPrincipal, GatewayError>) {
    let error = match result {
        Ok(_) => panic!("expected unauthorized"),
        Err(error) => error,
    };
    assert_eq!(error.status, 401);
}

#[test]
fn cookie_and_bearer_principals_map_to_the_passport_subject() -> TestResult {
    reset_jwks_cache();
    let token = token(&claims())?;
    let host = Host::new(vec![public_jwk("key-a")?], token.clone())?;
    let store = Store::default();
    let config = config();
    let auth = AuthService::new(&store, &host, &config);
    let cookie = with_cookie(
        request("GET", "https://ai.ubq.fi/uos/auth"),
        PASSPORT_COOKIE,
        &token,
    );
    let principal = run(auth.authenticate_client(&cookie)).must()?;
    assert!(matches!(
        principal.method,
        AuthMethod::PassportSession { .. }
    ));
    assert_eq!(
        principal.method.principal_id(None),
        "passport-user:user-123"
    );
    let mut bearer = request("GET", "https://ai.ubq.fi/uos/auth");
    bearer
        .headers
        .insert("authorization".into(), format!("Bearer {token}"));
    let principal = run(auth.authenticate_client(&bearer)).must()?;
    assert!(matches!(
        principal.method,
        AuthMethod::PassportBearer { .. }
    ));
    assert_eq!(
        principal.method.principal_id(None),
        "passport-user:user-123"
    );
    let response = run(auth.handle_auth_route(&cookie)).must()?.must()?;
    let body: Value = serde_json::from_slice(&response.body.to_bytes().must()?).must()?;
    assert_eq!(body["auth"]["principal"], "passport-user:user-123");
    assert_eq!(body["auth"]["method"]["kind"], "passport_session");
    assert_eq!(body["auth"]["token"]["present"], false);
    Ok(())
}

#[test]
fn invalid_jwt_shaped_bearer_never_falls_through_to_admin() -> TestResult {
    reset_jwks_cache();
    let host = Host::new(vec![public_jwk("key-a")?], token(&claims())?)?;
    let store = Store::default();
    let config = AuthConfig {
        admin_tokens: vec![gateway_auth::crypto::Secret::new("fixture-admin".into())],
        passport: Some(PassportConfig::production()),
        ..Default::default()
    };
    let auth = AuthService::new(&store, &host, &config);
    let bad = jwt(json!({"alg":"HS256","typ":"JWT","kid":"key-a"}), &claims())?;
    let mut request = request("GET", "https://ai.ubq.fi/uos/auth");
    request
        .headers
        .insert("authorization".into(), format!("Bearer {bad}"));
    assert_unauthorized(run(auth.authenticate_client(&request)));
    assert_unauthorized(run(auth.authenticate_admin(&request)));
    let mut valid_bearer = request.clone();
    valid_bearer.headers.insert(
        "authorization".into(),
        format!("Bearer {}", token(&claims())?),
    );
    assert!(run(auth.authenticate_client(&valid_bearer)).is_ok());
    assert_unauthorized(run(auth.authenticate_admin(&valid_bearer)));
    Ok(())
}

#[test]
fn admin_cookie_requires_isadmin_and_same_origin_mutations() -> TestResult {
    reset_jwks_cache();
    let admin_token = token(&claims())?;
    let host = Host::new(vec![public_jwk("key-a")?], admin_token.clone())?;
    let store = Store::default();
    let config = config();
    let auth = AuthService::new(&store, &host, &config);
    let get = with_cookie(
        request("GET", "https://ai.ubq.fi/admin"),
        PASSPORT_COOKIE,
        &admin_token,
    );
    assert!(run(auth.authenticate_admin(&get)).is_ok());
    let foreign = with_cookie(
        request("POST", "https://ai.ubq.fi/admin/api"),
        PASSPORT_COOKIE,
        &admin_token,
    );
    let mut foreign = foreign;
    foreign
        .headers
        .insert("origin".into(), "https://evil.example".into());
    assert_eq!(
        run(auth.authenticate_admin(&foreign)).err().must()?.status,
        403
    );
    let mut same = with_cookie(
        request("POST", "https://ai.ubq.fi/admin/api"),
        PASSPORT_COOKIE,
        &admin_token,
    );
    same.headers
        .insert("origin".into(), "https://ai.ubq.fi".into());
    assert!(run(auth.authenticate_admin(&same)).is_ok());

    let mut non_admin = claims();
    non_admin["isAdmin"] = json!(false);
    let non_admin = token(&non_admin)?;
    host.exchange.borrow_mut();
    *host.jwks.borrow_mut() = serde_json::to_vec(&json!({"keys":[public_jwk("key-a")?]})).must()?;
    let request = with_cookie(
        request("GET", "https://ai.ubq.fi/admin"),
        PASSPORT_COOKIE,
        &non_admin,
    );
    assert_eq!(
        run(auth.authenticate_admin(&request)).err().must()?.status,
        403
    );
    Ok(())
}

#[test]
fn same_origin_mutation_rejects_foreign_browser_markers() -> TestResult {
    let mut request = request("POST", "https://ai.ubq.fi/admin/api");
    assert!(same_origin_mutation(&request));
    request
        .headers
        .insert("sec-fetch-site".into(), "cross-site".into());
    assert!(!same_origin_mutation(&request));
    request.headers.remove("sec-fetch-site");
    request
        .headers
        .insert("origin".into(), "https://evil.example".into());
    assert!(!same_origin_mutation(&request));
    request.headers.remove("origin");
    request
        .headers
        .insert("referer".into(), "https://evil.example/page".into());
    assert!(same_origin_mutation(&request));
    Ok(())
}

#[test]
fn preview_client_config_is_explicit_and_fail_closed() -> TestResult {
    let config = AuthConfig::default();
    let candidate = request("GET", "https://rust-candidate.ai.ubq.fi/auth/login");
    assert!(gateway_auth::passport::config_for_request(&config, &candidate).is_none());
    let mut explicit = AuthConfig {
        passport: Some(PassportConfig {
            client_id: "candidate".into(),
            audience: "ai".into(),
            origin: "https://rust-candidate.ai.ubq.fi".into(),
            redirect_uri: "https://rust-candidate.ai.ubq.fi/auth/callback".into(),
        }),
        ..Default::default()
    };
    assert!(gateway_auth::passport::config_for_request(&explicit, &candidate).is_some());
    explicit.passport = None;
    let production = request("GET", "https://ai.ubq.fi/auth/login");
    assert_eq!(
        gateway_auth::passport::config_for_request(&config, &production),
        Some(PassportConfig::production())
    );
    Ok(())
}

#[test]
fn claims_serde_keeps_the_required_wire_metadata() -> TestResult {
    let parsed: PassportClaims = serde_json::from_value(claims()).must()?;
    assert_eq!(parsed.sub, "user-123");
    assert!(parsed.is_admin);
    assert_eq!(parsed.client_id, "ai");
    assert_eq!(parsed.agent_token_id, None);
    let shaped = gateway_auth::passport::method_json(&parsed, false);
    assert_eq!(shaped["principal"], "passport-user:user-123");
    Ok(())
}

#[test]
fn preview_config_never_defaults_to_the_production_client() -> TestResult {
    assert_eq!(PassportConfig::from_values(&BTreeMap::new()), None);
    let mut values = BTreeMap::from([
        ("UOS_AI_PASSPORT_CLIENT_ID".to_owned(), "preview".to_owned()),
        (
            "UOS_AI_PASSPORT_ORIGIN".to_owned(),
            "https://preview.example".to_owned(),
        ),
    ]);
    assert_eq!(PassportConfig::from_values(&values), None);
    values.insert(
        "UOS_AI_PASSPORT_REDIRECT_URI".to_owned(),
        "https://preview.example/auth/callback".to_owned(),
    );
    assert_eq!(
        PassportConfig::from_values(&values),
        Some(PassportConfig {
            client_id: "preview".into(),
            audience: "ai".into(),
            origin: "https://preview.example".into(),
            redirect_uri: "https://preview.example/auth/callback".into(),
        })
    );
    values.insert(
        "UOS_AI_PASSPORT_ORIGIN".to_owned(),
        "http://preview.example".to_owned(),
    );
    assert_eq!(PassportConfig::from_values(&values), None);
    Ok(())
}

#[test]
fn jwt_shape_requires_three_parts_and_an_algorithm_header() -> TestResult {
    let header = base64url(br#"{"alg":"RS256"}"#);
    assert!(jwt_shaped(&format!("{header}.payload.signature")));
    assert!(!jwt_shaped("opaque"));
    assert!(!jwt_shaped("a.b"));
    assert!(!jwt_shaped(".."));
    Ok(())
}
