mod external_support;
mod support;
use external_support::{Rig, command, credentials, fixture, group, identity, signed, snapshot};
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::state::AuthorityStore;
use gateway_native_external_publication_wire::receipt::SourceExternalPublicationDisposition as Disposition;
use support::{Cancel, Host, Store, TestResult, Tokens, run};
#[test]
fn real_source_external_effect_commits_all_four_rows_and_existing_lifecycle_verifies_terminal()
-> TestResult {
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
        let (action, claim, _) = rig.prepare("operation").await?;
        assert!(action.fence().principal.try_utf8().is_err());
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 61).await?;
        let keys = group(&action, "operation")?;
        let before = snapshot(&store, &keys).await?;
        let receipt = rig.service().publish_external(&proof, &body).await?;
        assert_eq!(receipt.disposition, Disposition::Applied);
        assert_eq!(receipt.claim_ref, claim.claim_ref());
        let after = snapshot(&store, &keys).await?;
        for (old, new) in before.iter().zip(&after) {
            assert_ne!(old.revision, new.revision);
        }
        assert_eq!(after[0].revision, "publication");
        assert_eq!(after[1].revision, "publication");
        assert_ne!(after[2].revision, after[3].revision);
        let operation: rows::OperationRow = serde_json::from_value(after[2].json_value()?.clone())?;
        let generation: rows::GenerationEffect =
            serde_json::from_value(after[3].json_value()?.clone())?;
        assert_eq!(operation.phase, rows::Phase::Published);
        let terminal = operation.terminal.ok_or("terminal")?;
        assert!(generation.terminal.is_some());
        assert_eq!(
            after[2].json_value()?["terminal"],
            after[3].json_value()?["terminal"]
        );
        assert_eq!(terminal.publication_revision, "publication");
        assert_eq!(terminal.native_source_generation, receipt.native_generation);
        assert_eq!(terminal.access_generation, receipt.native_generation);
        assert_eq!(
            terminal.publication_request_digest,
            Some(gateway_auth_primitives::crypto::sha256_hex(&body))
        );
        let managed = gateway_authority_native_managed::SourceNativeManagedService::new(
            &store,
            &tokens,
            Some(&host.now),
            &host,
            &config,
            gateway_authority_native_managed::SourceNativeManagedContext::configured(
                &id, &owner, &cancel,
            ),
        );
        let published = managed
            .verify_action_publication(&action, "operation", "publication")
            .await?;
        assert_eq!(published.principal, action.fence().principal);
        assert_eq!(published.source_revision, "publication");
        assert_eq!(
            rig.lifecycle()
                .published(&claim, "publication")
                .await?
                .publication_revision(),
            "publication"
        );
        let resolved = rig
            .lifecycle()
            .resolve_claim("operation", claim.claim_ref(), None)
            .await?;
        assert_eq!(resolved.publication_revision(), Some("publication"));
        let proof = signed(&store, &id, &body, 62).await?;
        let replay = rig.service().publish_external(&proof, &body).await?;
        assert_eq!(replay.disposition, Disposition::AlreadyCurrent);
        assert_eq!(snapshot(&store, &keys).await?, after);
        Ok(())
    })
}
#[test]
fn unchanged_external_acknowledges_new_identifier_and_retains_real_source_revisions() -> TestResult
{
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
        let (action, claim, _) = rig.prepare("unchanged").await?;
        let keys = group(&action, "unchanged")?;
        let before = snapshot(&store, &keys).await?;
        let body = command(
            &action,
            &claim,
            &credentials(300, "original")?,
            "acknowledged-new",
        )?;
        let proof = signed(&store, &id, &body, 63).await?;
        let receipt = rig.service().publish_external(&proof, &body).await?;
        assert_eq!(receipt.disposition, Disposition::AlreadyCurrent);
        assert_eq!(receipt.publication_revision, "acknowledged-new");
        assert_eq!(receipt.current_source_revision, "source");
        assert_eq!(receipt.current_normalized_revision, "normalized");
        let after = snapshot(&store, &keys).await?;
        assert_eq!(after[0], before[0]);
        assert_eq!(after[1], before[1]);
        assert_ne!(after[2].revision, before[2].revision);
        assert_ne!(after[3].revision, before[3].revision);
        assert_eq!(
            rig.lifecycle()
                .published(&claim, "acknowledged-new")
                .await?
                .publication_revision(),
            "acknowledged-new"
        );
        assert!(rig.lifecycle().published(&claim, "source").await.is_err());
        Ok(())
    })
}
#[test]
fn known_terminal_requires_exact_authenticated_body_digest_and_original_claim() -> TestResult {
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
        let (action, claim, _) = rig.prepare("replay").await?;
        let body = command(&action, &claim, &credentials(600, "next")?, "publication")?;
        let proof = signed(&store, &id, &body, 64).await?;
        rig.service().publish_external(&proof, &body).await?;
        let keys = group(&action, "replay")?;
        let before = snapshot(&store, &keys).await?;
        let mut changed = body.clone();
        changed.push(b'\n');
        let proof = signed(&store, &id, &changed, 65).await?;
        assert!(
            rig.service()
                .publish_external(&proof, &changed)
                .await
                .is_err()
        );
        for field in [
            "claim_ref",
            "publication_revision",
            "operation_id",
            "action_ref",
        ] {
            let mut changed: serde_json::Value = serde_json::from_slice(&body)?;
            changed[field] = serde_json::json!("foreign");
            let changed = serde_json::to_vec(&changed)?;
            let proof = signed(&store, &id, &changed, 66 + u8::try_from(nonce)?).await?;
            assert!(
                rig.service()
                    .publish_external(&proof, &changed)
                    .await
                    .is_err(),
                "{field}"
            );
        }
        assert_eq!(snapshot(&store, &keys).await?, before);
        Ok(())
    })
}
#[test]
fn reconcile_only_cross_original_marker_keeps_first_operation_ownership() -> TestResult {
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
        let (first, first_claim, _) = rig.prepare("first").await?;
        let (second, second_claim, _) = rig.prepare("second").await?;
        assert_eq!(
            second_claim.mode(),
            gateway_native_authority_wire::renewal::NativeRenewalMode::ReconcileOnly
        );
        assert!(second_claim.start_permit().is_none());
        let body = command(
            &second,
            &second_claim,
            &credentials(600, "next")?,
            "publication",
        )?;
        let proof = signed(&store, &id, &body, 67).await?;
        rig.service().publish_external(&proof, &body).await?;
        let key = group(&first, "first")?.pop().ok_or("generation key")?;
        let row = store.read(&key).await?.ok_or("generation")?;
        let generation: rows::GenerationEffect = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(generation.operation_id, "first");
        assert_eq!(generation.claim_ref, first_claim.claim_ref());
        assert_eq!(
            generation.terminal.ok_or("terminal")?.publication_revision,
            "publication"
        );
        Ok(())
    })
}
