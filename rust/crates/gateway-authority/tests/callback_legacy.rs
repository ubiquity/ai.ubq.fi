use gateway_auth_primitives::crypto::Secret;
use gateway_authority::native_control::actor::authenticate_client_actor;
use gateway_authority::store::Authority;
use gateway_authority_keys as keys;
use gateway_authority_test_support::native_actor::{
    Host, Store, config, context, request, run, session_setup,
};
use serde_json::{Value, json};
use std::sync::{
    Mutex,
    atomic::{AtomicUsize, Ordering},
};
type TestResult = Result<(), Box<dyn std::error::Error>>;
use gateway_auth_credentials::provider_credentials::{
    CodexNativeOwner, CodexSourceAccount, CodexSourcePool, MANAGED_NATIVE_SOURCE_VERSION,
    ManagedNativeSource, codex_source_pool_key,
};
use gateway_auth_native_contracts::transport::{NativeAuthHeaders, NativeCapability};
use gateway_auth_native_proof::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_wire::{
    control::{NativeControlEnvelope, NativeControlRequest},
    native_read::NATIVE_CONTROL_PATH,
};
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_authority_paid_control_records::{
    PaidControlConfig, PaidControlOwner, PaidSourceCheck,
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_authority_routing_selection::{SelectionOperations, types::LiveRouteSelection};
use gateway_contracts::{identity::PoolVersionstamp, model::ModelId};
use gateway_core::source_control::{decode_source_control_record, encode_source_control_record};
use gateway_kv_codec::{
    tuple_key::{ScalarKeyPart, TupleKey},
    value::ValueEncoding,
};
use gateway_native_authority_wire::{context::NativeRenewalContext, renewal::NativeRenewalMode};
use gateway_source::{js_string::JsString16, source_json::SourceJson};

mod callback_legacy_support;
use callback_legacy_support::{
    Alive, Nonces, Sequence, body, identity, mapping_key, owner_proof, publication_proof,
    runtime_key, seed_owned,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_authority_native_callback::execute_legacy;
use gateway_native_authority_request::{
    NativeRenewalAuthorityRequest as Callback, OriginalControlHeaders,
};
use gateway_native_authority_wire::renewal_response::NativeRenewalAuthorityResponse as CallbackResponse;

#[test]
fn legacy_callback_all_five_response1_status_errors_and_raw_mac() -> TestResult {
    run(async {
        let store = Store::default();
        let id = identity();
        seed_owned(&store, &id);
        let token = session_setup(&store, true, true);
        let auth =
            Authority::with_clock(store.clone(), Sequence(AtomicUsize::new(1)), store.clone());
        let cfg = config();
        let host = Host::new(&store);
        let actor = authenticate_client_actor(
            &auth,
            &host,
            &cfg,
            &request(&token),
            &context("passkey-user:user-fixture", None),
        )
        .await?;
        let LiveRouteSelection::Eligible(selected) =
            auth.select_live_account(None, 1_000_000).await?
        else {
            return Err("selection".into());
        };
        let owner = RegisteredRenewalOwner::configured(
            "native-owner".into(),
            "/tmp/ai-rust-wasm-20261018/synthetic-home".into(),
        )?;
        let alive = Alive;
        let service = NativeRenewalAuthority::configured(&auth, &id, &owner, &cfg, &alive);
        let action = service
            .prepare_live_create_renewal(&actor, &selected)
            .await?;
        let original = body(action.action_ref(), "callback-all-five");
        let nonce = gateway_auth_primitives::crypto::base64url(&[201; 32]);
        let claim = Callback::Claim {
            v: 1,
            original_control_body_base64url: gateway_auth_primitives::crypto::base64url(&original),
            original_auth_headers: OriginalControlHeaders {
                version: "1".into(),
                key_id: id.key_id().into(),
                time_ms: "1000000".into(),
                nonce: nonce.clone(),
                mac: id.sign(NATIVE_CONTROL_PATH, 1_000_000, &nonce, &original)?,
            },
        };
        let replay = StoreNonceAuthority::new(&store);
        let response =
            execute_legacy(&service, &id, &replay, &store, &serde_json::to_vec(&claim)?).await?;
        let CallbackResponse::Claim {
            v: 1,
            claim_ref,
            start_permit: Some(permit),
            publication_revision: None,
            mode: NativeRenewalMode::RenewOnce,
            ..
        } = response
        else {
            return Err("legacy claim".into());
        };
        let start = Callback::Start {
            v: 1,
            operation_id: "callback-all-five".into(),
            claim_ref: claim_ref.clone(),
            start_permit: permit,
        };
        assert!(matches!(
            execute_legacy(&service, &id, &replay, &store, &serde_json::to_vec(&start)?).await?,
            CallbackResponse::Start {
                v: 1,
                mode: NativeRenewalMode::RenewOnce
            }
        ));
        assert!(
            execute_legacy(&service, &id, &replay, &store, &serde_json::to_vec(&start)?)
                .await
                .is_err()
        );
        let recheck = Callback::Recheck {
            v: 1,
            operation_id: "callback-all-five".into(),
            claim_ref: claim_ref.clone(),
        };
        assert!(matches!(
            execute_legacy(
                &service,
                &id,
                &replay,
                &store,
                &serde_json::to_vec(&recheck)?
            )
            .await?,
            CallbackResponse::Recheck { v: 1, .. }
        ));
        let current = store
            .read(&gateway_auth_credentials::provider_credentials::codex_source_pool_key())
            .await?
            .ok_or("source")?;
        let source = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
            current.json_value()?,
        )?
        .ok_or("pool")?;
        let account_id = source
            .accounts
            .iter()
            .find(|a| a.identity().routing_account_hash.0 == action.fence().account_id_hash)
            .ok_or("selected account")?
            .account_id
            .clone();
        let access = format!(
            "{}.{}.synthetic",
            gateway_auth_primitives::crypto::base64url(b"{}"),
            gateway_auth_primitives::crypto::base64url(br#"{"exp":1600}"#)
        );
        let version = gateway_auth_credentials::provider_credentials::codex_credential_identity(
            &account_id,
            &Secret::new(access.clone()),
            &Secret::new(String::new()),
        );
        let fence = action.fence();
        let publication = serde_json::to_vec(
            &serde_json::json!({"operation":"managed_access_publish","v":2,"action_ref":action.action_ref(),"operation_id":"callback-all-five","native_owner_id":fence.native_owner_id,"owner_revision":fence.owner_revision,"source_revision":fence.source_revision,"normalized_revision":fence.normalized_revision,"publication_revision":"callback-publication","account_id_hash":fence.account_id_hash,"previous_native_source_generation":fence.source_native_generation,"native_source_generation":"b".repeat(64),"access_generation":version.native_generation_hash,"document":{"account_id":account_id,"access_token":access,"native_refreshed_at":null}}),
        )?;
        let receipt = service
            .publish_managed(
                &publication_proof(&id, &publication, 202).await,
                &publication,
            )
            .await?;
        let published = Callback::Published {
            v: 1,
            operation_id: "callback-all-five".into(),
            claim_ref: claim_ref.clone(),
            publication_revision: receipt.publication_revision,
        };
        let response = execute_legacy(
            &service,
            &id,
            &replay,
            &store,
            &serde_json::to_vec(&published)?,
        )
        .await?;
        assert!(matches!(response, CallbackResponse::Published { v: 1, .. }));
        let unknown = Callback::Unknown {
            v: 1,
            operation_id: "callback-all-five".into(),
            claim_ref,
        };
        let raw = serde_json::to_vec(&unknown)?;
        let response = execute_legacy(&service, &id, &replay, &store, &raw).await?;
        assert_eq!(
            serde_json::to_vec(&response)?,
            br#"{"operation":"unknown","v":1}"#
        );
        let signature = id.sign_authority_response(
            &gateway_auth_primitives::crypto::base64url(&[203; 32]),
            &raw,
            200,
            &serde_json::to_vec(&response)?,
        )?;
        id.verify_authority_response(
            &gateway_auth_primitives::crypto::base64url(&[203; 32]),
            &raw,
            200,
            &serde_json::to_vec(&response)?,
            &signature,
        )?;
        assert!(
            id.verify_authority_response(
                &gateway_auth_primitives::crypto::base64url(&[203; 32]),
                &raw,
                201,
                &serde_json::to_vec(&response)?,
                &signature
            )
            .is_err()
        );
        let error = execute_legacy(&service, &id, &replay, &store, b"{}")
            .await
            .err()
            .ok_or("malformed refused")?;
        assert_eq!(error.status, 400);
        let error_body = serde_json::to_vec(&error.envelope)?;
        let signature = id.sign_authority_response(
            &gateway_auth_primitives::crypto::base64url(&[204; 32]),
            b"{}",
            error.status,
            &error_body,
        )?;
        id.verify_authority_response(
            &gateway_auth_primitives::crypto::base64url(&[204; 32]),
            b"{}",
            error.status,
            &error_body,
            &signature,
        )?;
        Ok(())
    })
}
