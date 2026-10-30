//! Actual loopback TLS only. These synthetic roots never enter production configuration.
use super::*;
use gateway_native_https_test_support::import_tls::{Peer, roots};
#[test]
fn actual_import_models_tls_hostname_get_query_headers_and_no_redirect_are_fixed()
-> Result<(), Box<dyn std::error::Error>> {
    for redirect in [false, true] {
        let mut peer = Peer::start(true, redirect)?;
        let transport = PrivateHttps::models_connected(peer.socket()?, roots(true), "0.100.0")?;
        let response = transport.get_models(
            &[
                ("Authorization".into(), "Bearer SYNTHETIC_ACCESS".into()),
                ("ChatGPT-Account-ID".into(), "synthetic-account".into()),
            ],
            Instant::now() + Duration::from_secs(3),
        )?;
        assert_eq!(response.status, if redirect { 302 } else { 200 });
        let observed = peer
            .task
            .join()
            .map_err(|_| "Synthetic TLS fixture thread panicked")?
            .ok_or("Peer ended without an observation")?;
        assert_eq!(observed.sni, "chatgpt.com");
        assert_eq!(
            observed.line,
            "GET /backend-api/codex/models?client_version=0.100.0 HTTP/1.1"
        );
        assert_eq!(
            observed.headers.get("authorization").map(String::as_str),
            Some("Bearer SYNTHETIC_ACCESS")
        );
        assert_eq!(
            observed
                .headers
                .get("chatgpt-account-id")
                .map(String::as_str),
            Some("synthetic-account")
        );
        assert!(observed.body.is_empty());
    }
    // Same original cert/root succeeds for its actual localhost name, then fails
    // against the fixed models hostname; expiry/untrusted roots cannot explain this pair.
    let mut control = Peer::start(false, false)?;
    let transport = PrivateHttps::from_connected(
        "https://localhost/oauth/token",
        "/oauth/token",
        control.socket()?,
        roots(false),
    )?;
    assert_eq!(
        transport
            .send(&[], b"{}", Instant::now() + Duration::from_secs(3), 65_536)?
            .status,
        200
    );
    assert_eq!(
        control
            .task
            .join()
            .map_err(|_| "Synthetic TLS fixture thread panicked")?
            .ok_or("Peer ended without an observation")?
            .sni,
        "localhost"
    );
    let mut wrong = Peer::start(false, false)?;
    let transport = PrivateHttps::models_connected(wrong.socket()?, roots(false), "0.100.0")?;
    assert!(
        transport
            .get_models(&[], Instant::now() + Duration::from_secs(3))
            .is_err()
    );
    drop(transport);
    assert!(
        wrong
            .task
            .join()
            .map_err(|_| "Synthetic TLS fixture thread panicked")?
            .is_none()
    );
    Ok(())
}
#[test]
fn actual_import_oauth_tls_post_is_distinct_and_untrusted_roots_refuse()
-> Result<(), Box<dyn std::error::Error>> {
    let mut peer = Peer::start(true, false)?;
    let transport = PrivateHttps::from_connected(
        "https://auth.openai.com/oauth/token",
        "/oauth/token",
        peer.socket()?,
        roots(true),
    )?;
    assert!(
        transport
            .get_models(&[], Instant::now() + Duration::from_secs(3))
            .is_err()
    );
    let body = br#"{"client_id":"app_EMoamEEZ73f0CkXaXp7hrann","grant_type":"refresh_token","refresh_token":"SYNTHETIC_REFRESH","scope":"openid profile email"}"#;
    assert_eq!(
        transport
            .send(&[], body, Instant::now() + Duration::from_secs(3), 65_536)?
            .status,
        200
    );
    let observed = peer
        .task
        .join()
        .map_err(|_| "Synthetic TLS fixture thread panicked")?
        .ok_or("Peer ended without an observation")?;
    assert_eq!(observed.sni, "auth.openai.com");
    assert_eq!(observed.line, "POST /oauth/token HTTP/1.1");
    assert_eq!(observed.body, body);
    let mut peer = Peer::start(true, false)?;
    let transport =
        PrivateHttps::models_connected(peer.socket()?, ureq::tls::RootCerts::WebPki, "0.100.0")?;
    assert!(
        transport
            .get_models(&[], Instant::now() + Duration::from_secs(3))
            .is_err()
    );
    drop(transport);
    assert!(
        peer.task
            .join()
            .map_err(|_| "Synthetic TLS fixture thread panicked")?
            .is_none()
    );
    Ok(())
}
