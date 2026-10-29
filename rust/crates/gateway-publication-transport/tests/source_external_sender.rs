use gateway_auth_credentials::native_credentials::parse_native_document;
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::native_read::{NATIVE_NONCE_HEADER, NATIVE_RESPONSE_MAC_HEADER};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_external_publication_wire::{
    command::{ExpectedSourceExternalAccessPublication, PreparedSourceExternalAccessCommand},
    request::SourceExternalAccessRequest,
};
use gateway_publication_policy::protocol::{
    PublicationCarrier, PublicationRequest, PublicationResponse,
};
use gateway_publication_transport::source_external::send_source_external;
use gateway_source::error::GatewayError;
use serde_json::json;
use std::{
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
fn command() -> Result<PreparedSourceExternalAccessCommand, GatewayError> {
    let payload=serde_json::to_vec(&json!({"exp":1800000600,"https://api.openai.com/auth":{"chatgpt_user_id":"user","chatgpt_account_id":"account"}})).unwrap();
    let access = format!("e30.{}.fixture", base64url(&payload));
    let observed = parse_native_document(
        &json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":"account","access_token":access,"id_token":access,"refresh_token":""}}),
    )?;
    let fence=serde_json::from_value(json!({"v":3,"owner_id":"owner","owner_epoch":"epoch","owner_revision":"owner-revision","native_owner_id":"native","source_revision":"source","normalized_revision":"normalized","account_id_hash":observed.routing_account_hash(),"credential_version":"a".repeat(64),"source_native_generation":"a".repeat(64),"access_generation":"a".repeat(64),"access_expires_at_ms":null,"native_refreshed_at":null,"principal":"principal","principal_policy_digest":"policy","original_request_id":"original","context":{"kind":"live_mapped","call_id":"call","mapping_revision":"mapping"}})).unwrap();
    PreparedSourceExternalAccessCommand::from_registered(
        ExpectedSourceExternalAccessPublication {
            fence: &fence,
            action_ref: "action",
            operation_id: "operation",
            claim_ref: "claim",
            publication_revision: "ack-new",
        },
        &observed,
    )
}
struct Carrier<'a> {
    identity: &'a NativeTransportIdentity,
    mode: u8,
}
impl PublicationCarrier for Carrier<'_> {
    async fn send(&self, request: PublicationRequest) -> Result<PublicationResponse, GatewayError> {
        let parsed = SourceExternalAccessRequest::parse(&request.raw_body)?;
        let mut value = serde_json::to_value(&parsed).unwrap();
        let object = value.as_object_mut().unwrap();
        object.remove("document");
        object.insert("v".into(), json!(2));
        object.insert("owner_id".into(), json!(self.identity.owner_id()));
        object.insert("owner_epoch".into(), json!(self.identity.owner_epoch()));
        object.insert("disposition".into(), json!("already_current"));
        object.insert("current_source_revision".into(), json!("older-source"));
        object.insert(
            "current_normalized_revision".into(),
            json!("older-normalized"),
        );
        if self.mode == 2 {
            value["claim_ref"] = json!("foreign");
        }
        let status = if self.mode == 3 { 503 } else { 200 };
        let body = if status == 503 {
            serde_json::to_vec(
                &GatewayError::unavailable("unavailable", "fixture_unavailable").envelope,
            )
            .unwrap()
        } else if self.mode == 4 {
            b"not-json".to_vec()
        } else {
            serde_json::to_vec(&value).unwrap()
        };
        let nonce = request
            .headers
            .pairs()
            .into_iter()
            .find(|(name, _)| name == NATIVE_NONCE_HEADER)
            .unwrap()
            .1;
        let mac =
            self.identity
                .sign_publication_response(&nonce, &request.raw_body, status, &body)?;
        let mut headers = vec![(NATIVE_RESPONSE_MAC_HEADER.into(), mac.clone())];
        if self.mode == 1 {
            headers.push((NATIVE_RESPONSE_MAC_HEADER.into(), mac));
        }
        Ok(PublicationResponse {
            status,
            headers,
            raw_body: body,
        })
    }
}
#[test]
fn signed_sender_keeps_new_acknowledgment_distinct_from_actual_current_revision() -> TestResult {
    run(async {
        let identity = NativeTransportIdentity::new(
            "key".into(),
            "owner".into(),
            "epoch".into(),
            [73; 32],
            [NativeCapability::AccessPublish].into_iter().collect(),
        )?;
        let command = command()?;
        let receipt = send_source_external(
            &identity,
            &Carrier {
                identity: &identity,
                mode: 0,
            },
            &command,
            [1; 32],
            1_800_000_000_000,
        )
        .await?;
        assert_eq!(receipt.publication_revision, "ack-new");
        assert_eq!(receipt.current_source_revision, "older-source");
        Ok(())
    })
}
#[test]
fn sender_authenticates_exact_status_request_body_before_receipt_or_error_decode() -> TestResult {
    run(async {
        let identity = NativeTransportIdentity::new(
            "key".into(),
            "owner".into(),
            "epoch".into(),
            [73; 32],
            [NativeCapability::AccessPublish].into_iter().collect(),
        )?;
        let command = command()?;
        for mode in 1..5 {
            assert!(
                send_source_external(
                    &identity,
                    &Carrier {
                        identity: &identity,
                        mode
                    },
                    &command,
                    [mode; 32],
                    1_800_000_000_000
                )
                .await
                .is_err()
            );
        }
        Ok(())
    })
}
