//! Actual TLS and response authentication only, never a durable completion issuer.
use super::*;
use gateway_auth::native_import::publication::NativeImportPublicationDisposition;
use gateway_auth_native_wire::publication_data::ImportPublicationDataV1;
use gateway_core::source_json::SourceJson;
use gateway_native_https_test_support::publication::{Peer, PendingPeer, Response};
use std::{collections::BTreeSet, time::Duration};

type FixtureResult<T> = Result<T, Box<dyn std::error::Error>>;

#[derive(Debug)]
struct FixtureThreadFailure(Box<dyn std::any::Any + Send>);
impl std::fmt::Display for FixtureThreadFailure {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "{:?}", self.0)
    }
}
impl std::error::Error for FixtureThreadFailure {}

fn identity() -> NativeTransportIdentity {
    NativeTransportIdentity::new(
        "publication-fixture".into(),
        "owner".into(),
        "epoch".into(),
        [43; 32],
        BTreeSet::from([gateway_auth::native_auth::NativeCapability::CodexAuthImport]),
    )
    .unwrap()
}
fn command(limited: bool) -> ImportPublicationCommandData {
    let credentials = codex_credential_identity(
        "synthetic-account",
        &Secret::new("SYNTHETIC_ACCESS".into()),
        &Secret::new(String::new()),
    );
    ImportPublicationCommandData {
        operation: "codex_auth_import_publish".into(),
        v: 1,
        action_ref: "a".repeat(64),
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        native_owner_id: "owner".into(),
        owner_revision: "owner-r1".into(),
        source_revision: "source-r1".into(),
        normalized_revision: "normalized-r1".into(),
        publication_revision: "publication-r1".into(),
        account_id_hash: credentials.routing_account_hash.0,
        previous_native_source_generation: "c".repeat(64),
        native_source_generation: "d".repeat(64),
        access_generation: credentials.native_generation_hash,
        native_receipt_ref: "receipt-r1".into(),
        document: ImportAccessDocument {
            account_id: "synthetic-account".into(),
            access_token: "SYNTHETIC_ACCESS".into(),
            updated_at_ms: 1_000.0,
            native_refreshed_at: None,
        },
        catalog: if limited {
            ImportCatalogData::AuthenticatedLimited {
                upstream_status: 429,
            }
        } else {
            ImportCatalogData::Normalized {
                snapshot: SourceJson::parse(r#"{"models":[{"slug":"synthetic"}]}"#).unwrap(),
                client_version: "0.163.0".into(),
                upstream_status: 200,
            }
        },
    }
}
fn reply(command: &ImportPublicationCommandData) -> NativeImportPublication {
    let version = codex_credential_identity(
        &command.document.account_id,
        &Secret::new(command.document.access_token.clone()),
        &Secret::new(String::new()),
    );
    let revision = command.publication_revision.clone();
    NativeImportPublication {
        v: 1,
        operation_id: command.operation_id.clone(),
        action_ref: command.action_ref.clone(),
        disposition: NativeImportPublicationDisposition::Applied,
        publication: ImportPublicationDataV1 {
            v: 1,
            operation_id: command.operation_id.clone(),
            claim_ref: command.claim_ref.clone(),
            publication_revision: revision.clone(),
            native_receipt_ref: command.native_receipt_ref.clone(),
            native_source_generation: command.native_source_generation.clone(),
            access_generation: command.access_generation.clone(),
            credential_version: version.credential_version.0,
            source_revision: revision.clone(),
            normalized_revision: revision.clone(),
            catalog_revision: Some(revision.clone()),
            runtime_revision: Some(revision.clone()),
            auth_generation_revision: Some(revision.clone()),
            paid_control_config_revision: Some(revision),
            publication_request_digest: sha256_hex(&command.bytes().unwrap()),
            catalog_disposition: command.catalog.disposition(),
        },
    }
}
fn start_peer(command: &ImportPublicationCommandData, response: Response) -> FixtureResult<Peer> {
    let peer = PendingPeer::bind()?;
    let expected = command.bytes()?;
    let mut publication = reply(command);
    if matches!(response, Response::LargeValid) {
        assert!(matches!(
            command.catalog,
            ImportCatalogData::AuthenticatedLimited { .. }
        ));
        let retained = "\u{1}".repeat(4096);
        publication.publication.catalog_revision = Some(retained.clone());
        publication.publication.runtime_revision = Some(retained.clone());
        publication.publication.auth_generation_revision = Some(retained.clone());
        publication.publication.paid_control_config_revision = Some(retained);
    }
    let valid_reply = serde_json::to_vec(&publication)?;
    NativeImportPublication::parse_for(&valid_reply, &expected)?;
    if matches!(response, Response::LargeValid) {
        assert!(valid_reply.len() > 65_536);
    }
    Ok(peer.start(expected, valid_reply, identity(), response))
}
#[test]
fn actual_signed_publication_reply_matches_original_bytes_and_retains_old_limits()
-> FixtureResult<()> {
    for (limited, response) in [
        (false, Response::Valid),
        (true, Response::Valid),
        (true, Response::LargeValid),
    ] {
        let command = command(limited);
        let Peer {
            socket,
            task,
            listener,
        } = start_peer(&command, response)?;
        let sender = ImportPublicationHttp::fixture(
            "https://localhost/uos/native/publication",
            socket,
            identity(),
            include_bytes!("../../../tests/fixtures/native-tls/ca.der"),
        )?;
        let result = sender.exchange_data(&command, Instant::now() + Duration::from_secs(3))?;
        assert_eq!(result.request_digest(), sha256_hex(&command.bytes()?));
        assert_eq!(
            result.publication().publication.native_receipt_ref,
            command.native_receipt_ref
        );
        let observed = task
            .join()
            .map_err(FixtureThreadFailure)?
            .ok_or("Peer ended without an observation")?;
        assert_eq!(observed.sni, "localhost");
        assert_eq!(observed.line, "POST /uos/native/publication HTTP/1.1");
        assert_eq!(observed.body, command.bytes()?);
        assert_eq!(
            observed.headers.get(NATIVE_KEY_HEADER).map(String::as_str),
            Some("publication-fixture")
        );
        assert!(
            sender
                .exchange_data(&command, Instant::now() + Duration::from_millis(100))
                .is_err()
        );
        listener.set_nonblocking(true)?;
        assert!(
            matches!(listener.accept(), Err(error) if error.kind() == std::io::ErrorKind::WouldBlock)
        );
    }
    Ok(())
}
#[test]
fn actual_unsigned_dropped_redirect_and_oversized_error_never_issue_authenticated_reply()
-> FixtureResult<()> {
    for response in [
        Response::ForgedMac,
        Response::Dropped,
        Response::Redirect,
        Response::OversizedError,
    ] {
        let command = command(false);
        let Peer {
            socket,
            task,
            listener,
        } = start_peer(&command, response)?;
        let sender = ImportPublicationHttp::fixture(
            "https://localhost/uos/native/publication",
            socket,
            identity(),
            include_bytes!("../../../tests/fixtures/native-tls/ca.der"),
        )?;
        assert!(
            sender
                .exchange_data(&command, Instant::now() + Duration::from_secs(3))
                .is_err()
        );
        assert!(task.join().map_err(FixtureThreadFailure)?.is_some());
        assert!(
            sender
                .exchange_data(&command, Instant::now() + Duration::from_millis(100))
                .is_err()
        );
        listener.set_nonblocking(true)?;
        assert!(
            matches!(listener.accept(), Err(error) if error.kind() == std::io::ErrorKind::WouldBlock)
        );
    }
    Ok(())
}

#[test]
fn actual_ordinary_private_https_reader_keeps_65536_even_for_a_new_import_body() -> FixtureResult<()>
{
    let command = command(true);
    let body = command.bytes()?;
    let Peer {
        socket,
        task,
        listener: _,
    } = start_peer(&command, Response::LargeValid)?;
    let transport = PrivateHttps::from_connected(
        "https://localhost/uos/native/publication",
        NATIVE_PUBLICATION_PATH,
        socket,
        ureq::tls::RootCerts::new_with_certs(&[ureq::tls::Certificate::from_der(include_bytes!(
            "../../../tests/fixtures/native-tls/ca.der"
        ))
        .to_owned()]),
    )?;
    let nonce = base64url(&[7; 32]);
    let now = gateway_native_http_framing::response::now_ms()?;
    let id = identity();
    let headers = vec![
        (NATIVE_VERSION_HEADER.into(), "1".into()),
        (NATIVE_KEY_HEADER.into(), id.key_id().into()),
        (NATIVE_TIME_HEADER.into(), now.to_string()),
        (NATIVE_NONCE_HEADER.into(), nonce.clone()),
        (
            NATIVE_MAC_HEADER.into(),
            id.sign(NATIVE_PUBLICATION_PATH, now, &nonce, &body)?,
        ),
    ];
    assert!(
        transport
            .send(
                &headers,
                &body,
                Instant::now() + Duration::from_secs(3),
                65_536
            )
            .is_err()
    );
    assert!(task.join().map_err(FixtureThreadFailure)?.is_some());
    Ok(())
}
