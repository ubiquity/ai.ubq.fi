mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
use gateway_auth_native_contracts::transport::NativeAuthHeaders;
use gateway_auth_native_proof::native_auth::{
    AuthenticatedNativeOwner, NativeAuthenticator, NativeTransportIdentity,
};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH};
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_keys as keys;
use gateway_authority_native_callback::execute_source;
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_native_authority_request::{
    NativeRenewalAuthorityRequest as Request, OriginalControlHeaders,
};
use gateway_native_authority_wire::{
    renewal::NativeRenewalMode, source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use support::{
    Cancel, Host, NOW, Store, TestResult, Tokens, actor, control_body, fixture, identity, run,
};

async fn authority_peer(
    id: &NativeTransportIdentity,
    replay: &StoreNonceAuthority<'_, Store>,
    body: &[u8],
    now: i64,
    n: u8,
) -> Result<AuthenticatedNativeOwner, gateway_source::error::GatewayError> {
    let nonce = base64url(&[n; 32]);
    let mac = id.sign(NATIVE_RENEWAL_AUTHORITY_PATH, now, &nonce, body)?;
    NativeAuthenticator::new(id, replay)
        .authenticate(
            NATIVE_RENEWAL_AUTHORITY_PATH,
            NativeAuthHeaders {
                version: "1",
                key_id: id.key_id(),
                time_ms: &now.to_string(),
                nonce: &nonce,
                mac: &mac,
            },
            body,
            now,
        )
        .await
}

#[test]
fn closed_source_callback_emits_response_two_and_owned_unknown_needs_no_new_passport_or_pool()
-> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id, None)?;
        let (actor, token) = actor(&store, &host, &config).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let prep = SourceNativeRenewalService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativePreparationContext::configured(&id, &owner, &cancel),
        );
        let action = prep
            .prepare_source_live_mapped_renewal(&actor, "call")
            .await?;
        let service = SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
        );
        let replay = StoreNonceAuthority::new(&store);
        let original = control_body(action.action_ref(), "operation")?;
        let nonce = base64url(&[11; 32]);
        let request = Request::Claim {
            v: 1,
            original_control_body_base64url: base64url(&original),
            original_auth_headers: OriginalControlHeaders {
                version: "1".into(),
                key_id: id.key_id().into(),
                time_ms: NOW.to_string(),
                nonce: nonce.clone(),
                mac: id.sign(NATIVE_CONTROL_PATH, NOW, &nonce, &original)?,
            },
        };
        let raw = serde_json::to_vec(&request)?;
        let peer = authority_peer(&id, &replay, &raw, NOW, 12).await?;
        let Response::Claim {
            v: 2,
            claim_ref,
            start_permit: Some(permit),
            mode: NativeRenewalMode::RenewOnce,
            fence,
            ..
        } = execute_source(&service, &peer, &id, &replay, &host, &raw).await?
        else {
            return Err("source claim".into());
        };
        assert_eq!(fence.principal, *actor.principal().as_source());
        let raw = serde_json::to_vec(&Request::Start {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
            start_permit: permit,
        })?;
        let peer = authority_peer(&id, &replay, &raw, NOW, 13).await?;
        assert!(matches!(
            execute_source(&service, &peer, &id, &replay, &host, &raw).await?,
            Response::Start {
                v: 2,
                mode: NativeRenewalMode::RenewOnce
            }
        ));
        let raw = serde_json::to_vec(&Request::Recheck {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
        })?;
        let peer = authority_peer(&id, &replay, &raw, NOW, 14).await?;
        assert!(matches!(
            execute_source(&service, &peer, &id, &replay, &host, &raw).await?,
            Response::Recheck { v: 2, .. }
        ));
        assert!(
            execute_source(&service, &peer, &id, &replay, &host, b"{}")
                .await
                .is_err()
        );
        let raw = serde_json::to_vec(&Request::Published {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
            publication_revision: "not-published".into(),
        })?;
        let peer = authority_peer(&id, &replay, &raw, NOW, 15).await?;
        assert!(
            execute_source(&service, &peer, &id, &replay, &host, &raw)
                .await
                .is_err()
        );
        store.0.borrow_mut().remove(&codex_source_pool_key().0);
        store.0.borrow_mut().remove(&keys::pool().0);
        store
            .0
            .borrow_mut()
            .remove(&gateway_auth_passport_sessions::sessions::session_key(&token).0);
        host.status.set(401);
        host.now.set(NOW + 60_000);
        let calls = host.calls.get();
        let cancelled = Cancel(true);
        let cleanup = SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &cancelled),
        );
        let raw = serde_json::to_vec(&Request::Unknown {
            v: 1,
            operation_id: "operation".into(),
            claim_ref,
        })?;
        let peer = authority_peer(&id, &replay, &raw, host.now.get(), 16).await?;
        assert!(matches!(
            execute_source(&cleanup, &peer, &id, &replay, &host, &raw).await?,
            Response::Unknown { v: 2 }
        ));
        assert_eq!(host.calls.get(), calls);
        let row: rows::OperationRow = serde_json::from_value(
            store
                .0
                .borrow()
                .get(&rows::key("operation", "operation").0)
                .ok_or("operation")?
                .json_value()?
                .clone(),
        )?;
        assert_eq!(row.phase, rows::Phase::Unknown);
        Ok(())
    })
}
