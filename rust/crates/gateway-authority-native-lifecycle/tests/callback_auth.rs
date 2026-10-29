mod callback_support;
mod support;
use callback_support::{claim, decoded_claim, execute, peer};
use gateway_auth_contracts::types::AuthConfig;
use gateway_auth_native_contracts::transport::NativeCapability;
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{NATIVE_CONTROL_PATH, NATIVE_RENEWAL_AUTHORITY_PATH};
use gateway_authority_native_lifecycle::{
    SourceNativeLifecycleContext, SourceNativeLifecycleService,
};
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    renewal::NativeRenewalMode, source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, actor, fixture, identity, run};
#[test]
fn callback_outer_peer_epoch_fingerprint_path_body_nonce_and_replay_refuse() -> TestResult {
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
            Some(&host.now),
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
            Some(&host.now),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
        );
        let (claim_ref, permit) = decoded_claim(
            execute(
                &service,
                &store,
                &host,
                &id,
                &claim(&id, action.action_ref(), "operation", 101)?,
                1,
            )
            .await?,
        )?;
        let request = Request::Recheck {
            v: 1,
            operation_id: "operation".into(),
            claim_ref: claim_ref.clone(),
        };
        let bytes = serde_json::to_vec(&request)?;
        let replay = StoreNonceAuthority::new(&store);
        let calls = host.calls.get();
        for (index, (owner_id, epoch, key)) in [
            ("foreign", "epoch", [73; 32]),
            ("owner", "foreign", [73; 32]),
            ("owner", "epoch", [74; 32]),
        ]
        .into_iter()
        .enumerate()
        {
            let foreign = NativeTransportIdentity::new(
                "key".into(),
                owner_id.into(),
                epoch.into(),
                key,
                std::collections::BTreeSet::from([NativeCapability::OwnerRenewal]),
            )?;
            let proof = peer(
                &foreign,
                &replay,
                NATIVE_RENEWAL_AUTHORITY_PATH,
                &bytes,
                NOW,
                10 + index as u8,
            )
            .await?;
            assert!(
                gateway_authority_native_callback::execute_source(
                    &service, &proof, &id, &replay, &host.now, &bytes
                )
                .await
                .is_err()
            );
        }
        let wrong_path = peer(&id, &replay, NATIVE_CONTROL_PATH, &bytes, NOW, 14).await?;
        assert!(
            gateway_authority_native_callback::execute_source(
                &service,
                &wrong_path,
                &id,
                &replay,
                &host.now,
                &bytes
            )
            .await
            .is_err()
        );
        let proof = peer(&id, &replay, NATIVE_RENEWAL_AUTHORITY_PATH, &bytes, NOW, 15).await?;
        let mut changed = bytes.clone();
        changed.push(b' ');
        assert!(
            gateway_authority_native_callback::execute_source(
                &service, &proof, &id, &replay, &host.now, &changed
            )
            .await
            .is_err()
        );
        assert!(
            peer(&id, &replay, NATIVE_RENEWAL_AUTHORITY_PATH, &bytes, NOW, 15)
                .await
                .is_err()
        );
        assert_eq!(host.calls.get(), calls);
        assert!(matches!(
            gateway_authority_native_callback::execute_source(
                &service, &proof, &id, &replay, &host.now, &bytes
            )
            .await?,
            Response::Recheck { v: 2, .. }
        ));
        let _ = (token, permit);
        Ok(())
    })
}
#[test]
fn callback_nested_original_control_authentication_and_replay_refuse() -> TestResult {
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
            Some(&host.now),
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
            Some(&host.now),
            &host,
            &config,
            SourceNativeLifecycleContext::configured(&id, &owner, &cancel),
        );
        let (claim_ref, permit) = decoded_claim(
            execute(
                &service,
                &store,
                &host,
                &id,
                &claim(&id, action.action_ref(), "operation", 101)?,
                1,
            )
            .await?,
        )?;
        for (index, kind) in ["key", "path", "body", "time", "replay"]
            .into_iter()
            .enumerate()
        {
            let mut request = claim(
                &id,
                action.action_ref(),
                "another-operation",
                120 + index as u8,
            )?;
            if let Request::Claim {
                original_auth_headers: ref mut headers,
                ..
            } = request
            {
                match kind {
                    "key" => headers.key_id = "foreign-key".into(),
                    "path" => {
                        let body = support::control_body(action.action_ref(), "another-operation")?;
                        headers.mac =
                            id.sign(NATIVE_RENEWAL_AUTHORITY_PATH, NOW, &headers.nonce, &body)?;
                    }
                    "body" => {
                        headers.mac =
                            id.sign(NATIVE_CONTROL_PATH, NOW, &headers.nonce, b"different")?
                    }
                    "time" => headers.time_ms = (NOW - 120_000).to_string(),
                    "replay" => {
                        let original =
                            support::control_body(action.action_ref(), "another-operation")?;
                        let replay = StoreNonceAuthority::new(&store);
                        let _ = peer(
                            &id,
                            &replay,
                            NATIVE_CONTROL_PATH,
                            &original,
                            NOW,
                            120 + index as u8,
                        )
                        .await?;
                    }
                    _ => unreachable!(),
                }
            }
            let calls = host.calls.get();
            assert!(
                execute(&service, &store, &host, &id, &request, 30 + index as u8)
                    .await
                    .is_err()
            );
            assert_eq!(host.calls.get(), calls);
        }
        let _ = (token, claim_ref, permit);
        Ok(())
    })
}
