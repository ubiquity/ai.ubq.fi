use gateway_auth_credentials::{
    managed_access::ManagedAccessProjection,
    native_credentials::{
        NativeCredentials, NativeGeneration, NativeOwnerId, OwnerFence, parse_native_document,
    },
};
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_primitives::crypto::base64url;
use gateway_publication::{
    sender::NativePublicationSender,
    wire::{ExpectedAccessPublication, PreparedPublicationCommand},
};
use gateway_publication_policy::protocol::{
    PublicationCarrier, PublicationRequest, PublicationResponse,
};
use gateway_publication_transport::managed::{
    ExpectedManagedAccessPublication, PreparedManagedAccessCommand, send_managed,
};
use gateway_source::error::GatewayError;
use serde_json::json;
use std::{
    cell::RefCell,
    future::Future,
    task::{Context, Poll, Waker},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn run<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    let mut cx = Context::from_waker(Waker::noop());
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut cx) {
            return value;
        }
    }
}
fn identity() -> Result<NativeTransportIdentity, GatewayError> {
    NativeTransportIdentity::new(
        "key".into(),
        "owner".into(),
        "epoch".into(),
        [73; 32],
        [NativeCapability::AccessPublish].into_iter().collect(),
    )
}
fn credentials(external: bool) -> Result<NativeCredentials, GatewayError> {
    let payload=serde_json::to_vec(&json!({"exp":1800000600,"https://api.openai.com/auth":{"chatgpt_user_id":"user","chatgpt_account_id":"account"}})).unwrap();
    let access = format!("e30.{}.fixture", base64url(&payload));
    parse_native_document(
        &json!({"auth_mode":if external {"chatgptAuthTokens"}else{"chatgpt"},"OPENAI_API_KEY":null,"tokens":{"account_id":"account","access_token":access,"id_token":access,"refresh_token":if external {""}else{"retained-native-refresh"}}}),
    )
}
#[derive(Default)]
struct Capture(RefCell<Vec<u8>>);
impl PublicationCarrier for Capture {
    async fn send(&self, request: PublicationRequest) -> Result<PublicationResponse, GatewayError> {
        *self.0.borrow_mut() = request.raw_body;
        Err(GatewayError::unavailable(
            "capture complete",
            "fixture_capture_complete",
        ))
    }
}
#[test]
fn external_v1_complete_wire_bytes_are_unchanged() -> TestResult {
    run(async {
        let observed = credentials(true)?;
        let command = PreparedPublicationCommand::access_from_registered(
            ExpectedAccessPublication {
                native_owner_id: "native".into(),
                owner_revision: "owner".into(),
                source_revision: "source".into(),
                normalized_revision: Some("normalized".into()),
                account_id_hash: observed.routing_account_hash(),
                user_id: "user".into(),
                publication_revision: "publication".into(),
            },
            &observed,
        )?;
        let capture = Capture::default();
        let id = identity()?;
        let error = NativePublicationSender::configured(&id, &capture)
            .send(&command, [3; 32], 1_800_000_000_000)
            .await
            .err()
            .ok_or("capture")?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("fixture_capture_complete")
        );
        let expected = format!(
            r#"{{"operation":"access_publish","v":1,"native_owner_id":"native","owner_revision":"owner","source_revision":"source","normalized_revision":"normalized","publication_revision":"publication","account_id_hash":"{}","credential_version":"{}","native_generation":"{}","document":{{"account_id":"account","access_token":"{}","user_id":"user","native_refreshed_at":null}}}}"#,
            observed.routing_account_hash(),
            observed.routing_credential_version(),
            observed.generation_hash(),
            observed.access_token.expose()
        );
        assert_eq!(capture.0.borrow().as_slice(), expected.as_bytes());
        assert!(!expected.contains("claim_ref"));
        Ok(())
    })
}
#[test]
fn managed_v2_complete_wire_bytes_and_distinct_physical_generation_are_unchanged() -> TestResult {
    run(async {
        let native = credentials(false)?;
        let generation = NativeGeneration {
            fence: OwnerFence {
                owner: NativeOwnerId("native".into()),
                account_id: "account".into(),
                generation_hash: native.generation_hash(),
                authority_revision: "authority".into(),
            },
            credentials: native,
            observed_at_ms: 1_800_000_000_000,
        };
        let projection = ManagedAccessProjection::from_native(&generation)?;
        let command = PreparedManagedAccessCommand::managed_access_from_registered(
            ExpectedManagedAccessPublication {
                action_ref: "action:managed".into(),
                operation_id: "operation:managed".into(),
                native_owner_id: "native".into(),
                owner_revision: "owner".into(),
                source_revision: "source".into(),
                normalized_revision: "normalized".into(),
                publication_revision: "publication".into(),
                account_id_hash: generation.credentials.routing_account_hash(),
                previous_native_source_generation: "a".repeat(64),
            },
            &projection,
        )?;
        let id = identity()?;
        let capture = Capture::default();
        let error = send_managed(&id, &capture, &command, [4; 32], 1_800_000_000_000)
            .await
            .err()
            .ok_or("capture")?;
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("fixture_capture_complete")
        );
        let expected = format!(
            r#"{{"operation":"managed_access_publish","v":2,"action_ref":"action:managed","operation_id":"operation:managed","native_owner_id":"native","owner_revision":"owner","source_revision":"source","normalized_revision":"normalized","publication_revision":"publication","account_id_hash":"{}","previous_native_source_generation":"{}","native_source_generation":"{}","access_generation":"{}","document":{{"account_id":"account","access_token":"{}","native_refreshed_at":null}}}}"#,
            generation.credentials.routing_account_hash(),
            "a".repeat(64),
            projection.native_source_generation(),
            projection.access_generation(),
            projection.access_token().expose()
        );
        assert_eq!(capture.0.borrow().as_slice(), expected.as_bytes());
        assert_ne!(
            projection.native_source_generation(),
            projection.access_generation()
        );
        assert!(!expected.contains("claim_ref"));
        Ok(())
    })
}
