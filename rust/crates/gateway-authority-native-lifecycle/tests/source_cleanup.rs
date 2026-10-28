mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_native_proof::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use support::{
    Cancel, Host, NOW, Store, TestResult, Tokens, actor, control_body, fixture, identity, run,
    seed, signed_owner,
};
use support::{Nonces, unavailable};
fn unknown_body(operation: &str, claim: &str) -> Result<Vec<u8>, GatewayError> {
    serde_json::to_vec(
        &gateway_native_authority_request::NativeRenewalAuthorityRequest::Unknown {
            v: 1,
            operation_id: operation.into(),
            claim_ref: claim.into(),
        },
    )
    .map_err(|_| unavailable())
}
async fn signed_unknown(
    identity: &NativeTransportIdentity,
    body: &[u8],
    nonce_byte: u8,
    now: i64,
) -> Result<gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner, GatewayError> {
    use gateway_auth_native_proof::native_auth::{NativeAuthHeaders, NativeAuthenticator};
    let path = gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH;
    let nonce = gateway_auth_primitives::crypto::base64url(&[nonce_byte; 32]);
    let mac = identity.sign(path, now, &nonce, body)?;
    NativeAuthenticator::new(identity, &Nonces::default())
        .authenticate(
            path,
            NativeAuthHeaders {
                version: "1",
                key_id: identity.key_id(),
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
fn authenticated_unknown_refuses_wrong_peer_body_operation_claim_then_settles_after_revocation_cancel_deadline()
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
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let control = control_body(action.action_ref(), "operation")?;
        let peer = signed_owner(&id, &control, 20).await?;
        let mut claim = service
            .claim(&peer, action.action_ref(), "operation", &control)
            .await?;
        service.start(&mut claim).await?;
        let body = unknown_body("operation", claim.claim_ref())?;
        let proof = signed_unknown(&id, &body, 21, NOW).await?;
        let mut altered = body.clone();
        altered.push(b' ');
        assert!(service.record_unknown(&proof, &altered).await.is_err());
        let other = NativeTransportIdentity::new(
            "other".into(),
            "other".into(),
            "other".into(),
            [74; 32],
            std::collections::BTreeSet::from([NativeCapability::OwnerRenewal]),
        )?;
        let wrong = signed_unknown(&other, &body, 22, NOW).await?;
        assert!(service.record_unknown(&wrong, &body).await.is_err());
        for (operation, reference, n) in
            [("other", claim.claim_ref(), 23), ("operation", "other", 24)]
        {
            let wrong = unknown_body(operation, reference)?;
            let proof = signed_unknown(&id, &wrong, n, NOW).await?;
            assert!(service.record_unknown(&proof, &wrong).await.is_err());
        }
        let row = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        let before: rows::OperationRow = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(before.phase, rows::Phase::Started);
        store.0.borrow_mut().remove(&session_key(&token).0);
        host.now.set(NOW + 50_000);
        let cancelled = Cancel(true);
        let cleanup = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancelled,
            ),
        );
        let proof = signed_unknown(&id, &body, 25, host.now.get()).await?;
        let calls = host.calls.get();
        cleanup.record_unknown(&proof, &body).await?;
        cleanup.record_unknown(&proof, &body).await?;
        assert_eq!(host.calls.get(), calls);
        let row = store
            .read(&rows::key("operation", "operation"))
            .await?
            .ok_or("operation")?;
        let after: rows::OperationRow = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(after.phase, rows::Phase::Unknown);
        assert_eq!(after.claim_ref, before.claim_ref);
        assert!(after.terminal.is_none());
        Ok(())
    })
}
#[test]
fn authenticated_unknown_preserves_both_recorded_terminal_and_publication_without_minting_a_result()
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
        let service = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &tokens,
            Some(&host),
            &host,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let control = control_body(action.action_ref(), "operation")?;
        let peer = signed_owner(&id, &control, 26).await?;
        let claim = service
            .claim(&peer, action.action_ref(), "operation", &control)
            .await?;
        let operation_key = rows::key("operation", "operation");
        let raw = store.read(&operation_key).await?.ok_or("operation")?;
        let mut operation: rows::OperationRow = serde_json::from_value(raw.json_value()?.clone())?;
        let generation_key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let raw = store.read(&generation_key).await?.ok_or("generation")?;
        let mut generation: rows::GenerationEffect =
            serde_json::from_value(raw.json_value()?.clone())?;
        let terminal = rows::TerminalPublication {
            publication_revision: "publication".into(),
            source_revision: "source-next".into(),
            normalized_revision: "normalized-next".into(),
            native_source_generation: "a".repeat(64),
            access_generation: "b".repeat(64),
            credential_version: "credential-next".into(),
            publication_request_digest: Some("request-digest".into()),
        };
        operation.phase = rows::Phase::Published;
        operation.publication_revision = Some(terminal.publication_revision.clone());
        operation.terminal = Some(terminal.clone());
        generation.publication_revision = Some(terminal.publication_revision.clone());
        generation.terminal = Some(terminal);
        seed(
            &store,
            operation_key.clone(),
            "terminal-operation",
            operation,
        )?;
        seed(
            &store,
            generation_key.clone(),
            "terminal-generation",
            generation,
        )?;
        let before_operation = store.read(&operation_key).await?.ok_or("operation")?;
        let before_generation = store.read(&generation_key).await?.ok_or("generation")?;
        store.0.borrow_mut().remove(&session_key(&token).0);
        let body = unknown_body("operation", claim.claim_ref())?;
        let proof = signed_unknown(&id, &body, 27, NOW).await?;
        let calls = host.calls.get();
        service.record_unknown(&proof, &body).await?;
        assert_eq!(host.calls.get(), calls);
        let after_operation = store.read(&operation_key).await?.ok_or("operation")?;
        let after_generation = store.read(&generation_key).await?.ok_or("generation")?;
        assert_eq!(before_operation.revision, after_operation.revision);
        assert_eq!(before_operation.value, after_operation.value);
        assert_eq!(before_generation.revision, after_generation.revision);
        assert_eq!(before_generation.value, after_generation.value);
        Ok(())
    })
}
