#![cfg(unix)]
//! Real accepted-socket import admission. No private context/proof constructor or fixture issuer.
use gateway_auth::{
    crypto::sha256_hex,
    native_import::NativeImportMetadata,
    passkeys::{self, PasskeyUser},
};
use gateway_authority::{native_control::import::AuthenticatedNativeImport, store::Authority};
use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use gateway_native::{
    import_ingress::receive_import_connection,
    native_import::{NativeImportContext, NativeImportHandler},
    owner_control::NativeControlContext,
};
use gateway_native_import_source::{
    body::{AuthReadFailure, check_auth_content_length},
    result::{NativeImportCompletion, NativeImportResult},
};
use gateway_native_import_test_support::{
    fixture::{Fixture, Store, Tokens, fail, now_ms},
    socket::SocketRequest,
};
use std::{
    collections::BTreeMap,
    io::Write,
    sync::atomic::{AtomicU64, Ordering},
};

const AUTH: &str = r#"{"tokens":{"access_token":"SYNTHETIC_NATIVE_ACCESS","refresh_token":"SYNTHETIC_NATIVE_REFRESH","account_id":"synthetic-account"}}"#;
fn receive(
    f: &Fixture,
    a: &Authority<Store, Tokens>,
    body: &[u8],
    consumer: &impl NativeImportHandler,
    revoked: bool,
) -> Result<NativeImportResult, GatewayError> {
    let mut socket = SocketRequest::prepared(f, a, body)?;
    if !revoked {
        socket.send(body)?;
        return receive_import_connection(
            &mut socket.accepted,
            a,
            &f.identity,
            &f.owner,
            &f.config,
            &socket.nonces,
            consumer,
        );
    }
    socket.send_head()?;
    let mut sender = socket.client.try_clone().map_err(|_| fail())?;
    std::thread::scope(|scope| {
        let task = scope.spawn(move || {
            receive_import_connection(
                &mut socket.accepted,
                a,
                &f.identity,
                &f.owner,
                &f.config,
                &socket.nonces,
                consumer,
            )
        });
        f.store.wait_header_admitted()?;
        f.store.seed(
            passkeys::key("users", "user-fixture"),
            "user-revoked",
            PasskeyUser {
                id: "user-fixture".into(),
                handle: "operator".into(),
                is_admin: false,
                credential_ids: vec!["credential".into()],
                created_at_ms: 1,
                updated_at_ms: now_ms()?,
            },
        )?;
        sender.write_all(body).map_err(|_| fail())?;
        task.join().map_err(|_| fail())?
    })
}
struct Consumer {
    auth: SourceJson,
    version: Option<JsString16>,
    raw: Vec<u8>,
    calls: AtomicU64,
}
impl Consumer {
    fn expected(version: Option<JsString16>, raw: &[u8]) -> Result<Self, GatewayError> {
        Ok(Self {
            auth: SourceJson::parse(AUTH).map_err(|_| fail())?,
            version,
            raw: raw.to_vec(),
            calls: AtomicU64::new(0),
        })
    }
}
impl NativeImportHandler for Consumer {
    fn import_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        raw: &[u8],
        context: &NativeImportContext<'_>,
        original: &NativeControlContext<'_>,
    ) -> Result<NativeImportResult, GatewayError> {
        context.require_original(original)?;
        context.check_document(proof, metadata, raw)?;
        assert_eq!(raw, self.raw);
        assert_eq!(context.auth_data().auth().as_str(), self.auth.as_str());
        assert_eq!(context.auth_data().client_version(), self.version.as_ref());
        self.calls.fetch_add(1, Ordering::SeqCst);
        Ok(NativeImportResult {
            status: 200,
            headers: BTreeMap::new(),
            body: SourceJson::parse(r#"{"native_data_consumed":true}"#).map_err(|_| fail())?,
            completion: NativeImportCompletion::Unavailable,
        })
    }
}
#[test]
fn actual_decoder_data_is_meaningful_handler_input_for_bare_wrapped_bom_and_utf16()
-> Result<(), GatewayError> {
    for (body, version) in [
        (AUTH.as_bytes().to_vec(), None),
        (
            format!(
                r#" {{"auth":{AUTH},"models":{{"client_version":"","clientVersion":"ignored"}}}} "#
            )
            .into_bytes(),
            Some(JsString16::from_units(vec![])),
        ),
        (
            format!(r#"{{"auth":{AUTH},"models":{{"clientVersion":"\ud800"}}}}"#).into_bytes(),
            Some(JsString16::from_units(vec![0xd800])),
        ),
        ([&[0xef, 0xbb, 0xbf][..], AUTH.as_bytes()].concat(), None),
    ] {
        let f = Fixture::new()?;
        let a = f.authority();
        let consumer = Consumer::expected(version, &body)?;
        let result = receive(&f, &a, &body, &consumer, false).map_err(|_| fail())?;
        assert_eq!(result.status, 200);
        assert_eq!(consumer.calls.load(Ordering::SeqCst), 1);
        let capture = f.store.captured()?;
        for forbidden in [
            "SYNTHETIC_NATIVE_ACCESS",
            "SYNTHETIC_NATIVE_REFRESH",
            &sha256_hex(&body),
        ] {
            assert!(!capture.contains(forbidden));
        }
    }
    Ok(())
}
#[test]
fn actual_bound_but_invalid_auth_uses_original_error_without_calling_handler()
-> Result<(), GatewayError> {
    for body in [
        b"{}".to_vec(),
        b"{".to_vec(),
        [&[0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf][..], AUTH.as_bytes()].concat(),
    ] {
        let f = Fixture::new()?;
        let a = f.authority();
        let consumer = Consumer::expected(None, &body)?;
        let error = match receive(&f, &a, &body, &consumer, false) {
            Err(e) => e,
            Ok(_) => panic!("invalid AUTH reached handler"),
        };
        assert_eq!(error.status, 400);
        assert_eq!(
            error.envelope.error.message,
            "Body does not look like a Codex auth.json"
        );
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("invalid_request_error")
        );
        assert_eq!(consumer.calls.load(Ordering::SeqCst), 0);
    }
    Ok(())
}
#[test]
fn current_actual_actor_revocation_is_checked_before_the_auth_decoder() -> Result<(), GatewayError>
{
    let f = Fixture::new()?;
    let a = f.authority();
    let body = b"{";
    let consumer = Consumer::expected(None, body)?;
    let error = match receive(&f, &a, body, &consumer, true) {
        Err(e) => e,
        Ok(_) => panic!("stale actor"),
    };
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_actor_unavailable")
    );
    assert_eq!(consumer.calls.load(Ordering::SeqCst), 0);
    Ok(())
}
#[test]
fn ingress_header_guard_and_error_mapping_are_the_original_source_rules() -> Result<(), GatewayError>
{
    for good in [None, Some("0"), Some("33554432")] {
        assert!(check_auth_content_length(good).is_ok());
    }
    for invalid in ["", "00", "1.0", "+1", " 1", "9007199254740992"] {
        assert_eq!(
            check_auth_content_length(Some(invalid)),
            Err(AuthReadFailure::Invalid)
        );
    }
    assert_eq!(
        check_auth_content_length(Some("33554433")),
        Err(AuthReadFailure::TooLarge)
    );

    for length in ["", "00", "1.0", "+1", "9007199254740992", "33554433"] {
        let f = Fixture::new()?;
        let a = f.authority();
        let consumer = Consumer::expected(None, AUTH.as_bytes())?;
        let mut socket = SocketRequest::prepared(&f, &a, AUTH.as_bytes())?;
        let head = String::from_utf8(socket.head.clone()).map_err(|_| fail())?;
        socket.head = head
            .replace(
                &format!("Content-Length: {}", AUTH.len()),
                &format!("Content-Length: {length}"),
            )
            .into_bytes();
        socket.send_head()?;
        let error = receive_import_connection(
            &mut socket.accepted,
            &a,
            &f.identity,
            &f.owner,
            &f.config,
            &socket.nonces,
            &consumer,
        )
        .err()
        .ok_or_else(fail)?;
        assert_eq!(error.status, 400);
        assert_eq!(
            error.envelope.error.message,
            "Body does not look like a Codex auth.json"
        );
        assert_eq!(error.envelope.error.error_type, "invalid_request_error");
        assert_eq!(consumer.calls.load(Ordering::SeqCst), 0);
    }
    Ok(())
}

#[test]
fn actual_original_disconnect_after_header_admission_precedes_any_decode_error()
-> Result<(), GatewayError> {
    let f = Fixture::new()?;
    let a = f.authority();
    let body = b"{";
    let consumer = Consumer::expected(None, body)?;
    let mut socket = SocketRequest::prepared(&f, &a, body)?;
    socket.send_head()?;
    let sender = socket.client.try_clone().map_err(|_| fail())?;
    std::thread::scope(|scope| {
        let task = scope.spawn(|| {
            receive_import_connection(
                &mut socket.accepted,
                &a,
                &f.identity,
                &f.owner,
                &f.config,
                &socket.nonces,
                &consumer,
            )
        });
        f.store.wait_header_admitted()?;
        sender
            .shutdown(std::net::Shutdown::Both)
            .map_err(|_| fail())?;
        let error = task.join().map_err(|_| fail())?.err().ok_or_else(fail)?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("native_control_outcome_unknown")
        );
        Ok::<(), GatewayError>(())
    })?;
    assert_eq!(consumer.calls.load(Ordering::SeqCst), 0);
    Ok(())
}
