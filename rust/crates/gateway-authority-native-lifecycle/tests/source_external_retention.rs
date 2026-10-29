mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::state::AuthorityStore;
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, run};
#[test]
fn already_advanced_usable_external_generation_publishes_without_consuming_start() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let rig = Rig {
            store: &store,
            host: &host,
            config: &config,
            tokens: &tokens,
            identity: &id,
            owner: &owner,
            cancel: &cancel,
        };
        let (actor, _) = support::actor(&store, &host, &config).await?;
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
        let control = support::control_body(action.action_ref(), "zero-start")?;
        let proof = support::signed_owner(&id, &control, 191).await?;
        let claim = rig
            .lifecycle()
            .claim(&proof, action.action_ref(), "zero-start", &control)
            .await?;
        assert!(claim.start_permit().is_some());
        let before = store
            .read(&rows::key("operation", "zero-start"))
            .await?
            .ok_or("operation")?;
        assert_eq!(before.json_value()?["phase"], "claimed");
        let body = command(
            &action,
            &claim,
            &credentials(600, "already-advanced-native")?,
            "publication",
        )?;
        let proof = signed(&store, &id, &body, 192).await?;
        rig.service().publish_external(&proof, &body).await?;
        let after = store
            .read(&rows::key("operation", "zero-start"))
            .await?
            .ok_or("operation")?;
        assert_eq!(after.json_value()?["phase"], "published");
        assert_eq!(
            rig.lifecycle()
                .published(&claim, "publication")
                .await?
                .publication_revision(),
            "publication"
        );
        Ok(())
    })
}
#[test]
fn owned_unknown_after_lost_source_and_session_preserves_a_known_terminal() -> TestResult {
    run(async {
        let store = Store::default();
        let host = Host::default();
        let config = AuthConfig::default();
        let tokens = Tokens::default();
        let id = identity()?;
        fixture(&store, &id).await?;
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let rig = Rig {
            store: &store,
            host: &host,
            config: &config,
            tokens: &tokens,
            identity: &id,
            owner: &owner,
            cancel: &cancel,
        };
        let (action, claim, token) = rig.prepare("retained").await?;
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 193).await?;
        rig.service().publish_external(&proof, &body).await?;
        let keys = group(&action, "retained")?;
        let before = snapshot(&store, &keys).await?;
        store.0.borrow_mut().remove(&keys[0].0);
        store
            .0
            .borrow_mut()
            .remove(&gateway_auth_passport_sessions::sessions::session_key(&token).0);
        let unknown = serde_json::to_vec(
            &gateway_native_authority_request::NativeRenewalAuthorityRequest::Unknown {
                v: 1,
                operation_id: "retained".into(),
                claim_ref: claim.claim_ref().into(),
            },
        )?;
        let path = gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH;
        let nonce = gateway_auth_primitives::crypto::base64url(&[194; 32]);
        let mac = id.sign(path, NOW, &nonce, &unknown)?;
        let replay = gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(&store);
        let proof = gateway_auth_native_proof::native_auth::NativeAuthenticator::new(&id, &replay)
            .authenticate(
                path,
                gateway_auth_native_contracts::transport::NativeAuthHeaders {
                    version: "1",
                    key_id: id.key_id(),
                    time_ms: &NOW.to_string(),
                    nonce: &nonce,
                    mac: &mac,
                },
                &unknown,
                NOW,
            )
            .await?;
        rig.lifecycle().record_unknown(&proof, &unknown).await?;
        for index in 2..4 {
            assert_eq!(
                store.read(&keys[index]).await?.ok_or("known terminal")?,
                before[index]
            );
        }
        Ok(())
    })
}
