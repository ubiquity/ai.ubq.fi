use gateway_auth::crypto::Secret;
use gateway_auth::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth::store;
use gateway_authority::keys;
use gateway_authority::paid_control::{PaidControlConfig, PaidControlOwner};
use gateway_core::provider::PaidProvider;
use gateway_core::state::AuthorityStore;
use gateway_publication::sender::NativePublicationSender;
use gateway_publication::wire::{PreparedPublicationCommand, PublicationDisposition};
use gateway_publication_policy::control_publication::{
    ConfiguredPaidInputs, PreparedConfigurationPublication, bootstrap_fixture_owner,
    configured_paid_binding, fixture_owner_statement, runtime_config_key,
};
use gateway_publication_test_support::fixture::{
    FixtureCarrier as LocalCarrier, FixtureMemory as Memory, fixture_bootstrap as bootstrap,
    fixture_expect_error, fixture_identity as make_identity, fixture_inputs as inputs,
    fixture_now as now, fixture_proof as proof,
    fixture_publication_identity as publication_identity, fixture_run as run, fixture_set as set,
};
use gateway_publication_transport::transport::PublicationReceiver;
use serde_json::json;
use std::collections::BTreeSet;
use std::error::Error;

#[test]
fn real_authenticated_constructor_publishes_only_derived_fixed_control_facts() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        let now = 1_792_339_000_000;
        bootstrap(&store, &identity, now).await;
        let inputs = inputs("fixture-surplus-key");
        let publication = PreparedConfigurationPublication::capture(
            &store,
            &identity,
            &inputs,
            "generation-1",
            now,
        )
        .await
        .unwrap();
        let signed = proof(&identity, publication.statement(), now).await;
        let configured = publication
            .publish(&store, &signed, now, "config-r1")
            .await
            .unwrap();
        assert_eq!(
            configured.surplus_binding,
            inputs.binding(PaidProvider::Surplus)
        );
        assert_eq!(
            configured.openlux_binding,
            inputs.binding(PaidProvider::Metered)
        );
        assert_eq!(configured.v, 2);
        assert_eq!(
            configured.openlux_discovery_binding,
            inputs.discovery_binding(PaidProvider::Metered)
        );
        assert_eq!(configured.source_checks.len(), 1);
        assert_eq!(configured.source_checks[0].key, runtime_config_key().0);
        assert_eq!(configured.source_checks[0].expected_revision, None);
        let rows = store.0.lock().unwrap();
        let persisted = serde_json::to_string(&*rows).unwrap();
        assert!(!persisted.contains("fixture-surplus-key"));
        assert!(!persisted.contains("fixture-openlux-key"));
        assert_eq!(rows.len(), 2);
    });
}

#[test]
fn explicit_discovery_and_spend_roles_have_independent_configuration_fingerprints() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        bootstrap(&store, &identity, now()).await;
        let mut configurations = Vec::new();
        for (discovery, spend) in [
            (Some("fixture-discovery-a"), Some("fixture-spend-a")),
            (Some("fixture-discovery-b"), Some("fixture-spend-a")),
            (Some("fixture-discovery-a"), Some("fixture-spend-b")),
            (None, Some("fixture-spend-a")),
            (Some("fixture-discovery-a"), None),
        ] {
            let inputs = ConfiguredPaidInputs::new(
                None,
                discovery.map(|key| Secret::new(key.into())),
                spend.map(|key| Secret::new(key.into())),
                json!({"overage_enabled":true}),
            )
            .unwrap();
            let prepared =
                PreparedConfigurationPublication::capture(&store, &identity, &inputs, "g1", now())
                    .await
                    .unwrap();
            let config = prepared.configuration().clone();
            assert_eq!(
                config.openlux_discovery_binding,
                inputs.discovery_binding(PaidProvider::Metered)
            );
            assert_eq!(
                config.openlux_binding,
                inputs.binding(PaidProvider::Metered)
            );
            configurations.push(config);
        }
        assert_eq!(
            configurations[0].openlux_binding,
            configurations[1].openlux_binding
        );
        assert_eq!(
            configurations[0].openlux_discovery_binding,
            configurations[2].openlux_discovery_binding
        );
        assert_ne!(
            configurations[0].openlux_discovery_binding,
            configurations[1].openlux_discovery_binding
        );
        assert_ne!(
            configurations[0].openlux_binding,
            configurations[2].openlux_binding
        );
        for config in &configurations[1..] {
            assert_ne!(
                configurations[0].configuration_fingerprint,
                config.configuration_fingerprint
            );
        }
        assert!(configurations[3].openlux_discovery_binding.is_none());
        assert!(configurations[3].openlux_binding.is_some());
        assert!(configurations[4].openlux_discovery_binding.is_some());
        assert!(configurations[4].openlux_binding.is_none());
        assert_eq!(
            configurations[0].openlux_binding,
            Some(configured_paid_binding(
                PaidProvider::Metered,
                &Secret::new("fixture-spend-a".into())
            ))
        );
        let same = inputs("fixture-surplus-key");
        assert_eq!(
            same.binding(PaidProvider::Metered),
            same.discovery_binding(PaidProvider::Metered)
        );
        for (discovery, spend) in [(Some(" "), Some("valid")), (Some("valid"), Some(""))] {
            assert!(
                ConfiguredPaidInputs::new(
                    None,
                    discovery.map(|key| Secret::new(key.into())),
                    spend.map(|key| Secret::new(key.into())),
                    json!({})
                )
                .is_err()
            );
        }
        let encoded = serde_json::to_string(&configurations).unwrap();
        for secret in [
            "fixture-discovery-a",
            "fixture-discovery-b",
            "fixture-spend-a",
            "fixture-spend-b",
        ] {
            assert!(!encoded.contains(secret));
        }
        let mut legacy = configurations[0].clone();
        legacy.v = 1;
        assert!(
            PreparedPublicationCommand::configuration_from_expected(
                legacy.clone(),
                "owner-r1".into(),
                None,
                "config-r1".into()
            )
            .is_err()
        );
        set(
            &store,
            keys::paid_control_config(),
            "legacy-config",
            serde_json::to_value(&legacy).unwrap(),
        );
        assert!(
            PreparedConfigurationPublication::capture(&store, &identity, &same, "g2", now())
                .await
                .is_err()
        );
        let mut missing = serde_json::to_value(&configurations[0]).unwrap();
        missing
            .as_object_mut()
            .unwrap()
            .remove("openlux_discovery_binding");
        set(&store, keys::paid_control_config(), "missing-role", missing);
        assert!(
            PreparedConfigurationPublication::capture(&store, &identity, &same, "g2", now())
                .await
                .is_err()
        );
    });
}

#[test]
fn discovery_only_change_invalidates_observations_and_old_authenticated_commands()
-> Result<(), Box<dyn Error>> {
    run(async {
        let store = Memory::default();
        let identity = publication_identity();
        bootstrap(&store, &identity, now()).await;
        let initial = inputs("fixture-surplus-key");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &initial, "g1", now())
                .await?;
        let original = prepared.configuration().clone();
        let signed = proof(&identity, prepared.statement(), now()).await;
        prepared.publish(&store, &signed, now(), "cfg1").await?;
        set(
            &store,
            keys::paid_control_observation(PaidProvider::Metered),
            "old-observation",
            json!({"obsolete":true}),
        );
        let next = ConfiguredPaidInputs::new(
            Some(Secret::new("fixture-surplus-key".into())),
            Some(Secret::new("fixture-next-discovery".into())),
            Some(Secret::new("fixture-openlux-key".into())),
            json!({"overage_enabled":true}),
        )?;
        let old_prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &initial, "g2", now())
                .await?;
        let old_command = gateway_publication::wire::configuration_command(&old_prepared, "cfg2")?;
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &next, "g2", now())
                .await?;
        assert_eq!(
            prepared.configuration().openlux_binding,
            original.openlux_binding
        );
        assert_ne!(
            prepared.configuration().openlux_discovery_binding,
            original.openlux_discovery_binding
        );
        let receiver = PublicationReceiver::configured(identity.clone(), next, vec![], now)?;
        let carrier = LocalCarrier {
            receiver: &receiver,
            store: &store,
            lose_response: std::cell::Cell::new(false),
            tamper_response: false,
        };
        let sender = NativePublicationSender::configured(&identity, &carrier);
        let error = fixture_expect_error(sender.send(&old_command, [21; 32], now()).await);
        assert_eq!(error.status, 409);
        let observation = store
            .read(&keys::paid_control_observation(PaidProvider::Metered))
            .await?;
        assert!(observation.is_some());
        let command = gateway_publication::wire::configuration_command(&prepared, "cfg2")?;
        sender.send(&command, [22; 32], now()).await?;
        let observation = store
            .read(&keys::paid_control_observation(PaidProvider::Metered))
            .await?;
        assert!(observation.is_none());
        let current = store
            .read(&keys::paid_control_config())
            .await?
            .ok_or_else(|| std::io::Error::other("fixture configuration row missing"))?;
        assert_eq!(current.revision, "cfg2");
        Ok::<(), Box<dyn Error>>(())
    })
}

#[test]
fn stale_prepared_owner_config_and_source_revisions_are_fenced() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        let now = 1_792_339_000_000;
        bootstrap(&store, &identity, now).await;
        let inputs = inputs("first-secret");
        let stale =
            PreparedConfigurationPublication::capture(&store, &identity, &inputs, "old", now)
                .await
                .unwrap();
        let stale_proof = proof(&identity, stale.statement(), now).await;
        let current =
            PreparedConfigurationPublication::capture(&store, &identity, &inputs, "current", now)
                .await
                .unwrap();
        let signed = proof(&identity, current.statement(), now).await;
        current
            .publish(&store, &signed, now, "current-r1")
            .await
            .unwrap();
        assert_eq!(
            stale
                .publish(&store, &stale_proof, now, "stale-r1")
                .await
                .unwrap_err()
                .status,
            409
        );
        let stale_source =
            PreparedConfigurationPublication::capture(&store, &identity, &inputs, "new", now)
                .await
                .unwrap();
        let signed = proof(&identity, stale_source.statement(), now).await;
        set(
            &store,
            runtime_config_key(),
            "runtime-r2",
            json!({"overage_enabled":false}),
        );
        assert_eq!(
            stale_source
                .publish(&store, &signed, now, "unused")
                .await
                .unwrap_err()
                .status,
            409
        );
        let stale_owner =
            PreparedConfigurationPublication::capture(&store, &identity, &inputs, "next", now)
                .await
                .unwrap();
        let signed = proof(&identity, stale_owner.statement(), now).await;
        set(
            &store,
            keys::paid_control_owner(),
            "owner-r2",
            json!({"v":1,"owner_id":"fixture-owner","owner_epoch":"replacement","key_fingerprint":identity.key_fingerprint()}),
        );
        assert_eq!(
            stale_owner
                .publish(&store, &signed, now, "unused")
                .await
                .unwrap_err()
                .status,
            409
        );
        let row = store
            .read(&keys::paid_control_config())
            .await
            .unwrap()
            .unwrap();
        let config: PaidControlConfig = serde_json::from_value(row.value).unwrap();
        assert_eq!(config.generation, "current");
    });
}

#[test]
fn changed_bindings_delete_obsolete_observations_in_the_same_cas() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        let now = 1_792_339_000_000;
        bootstrap(&store, &identity, now).await;
        let first = inputs("old-secret");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &first, "g1", now)
                .await
                .unwrap();
        let signed = proof(&identity, prepared.statement(), now).await;
        prepared
            .publish(&store, &signed, now, "cfg1")
            .await
            .unwrap();
        for provider in [PaidProvider::Surplus, PaidProvider::Metered] {
            set(
                &store,
                keys::paid_control_observation(provider),
                "old-observation",
                json!({"obsolete":true}),
            );
        }
        let next = inputs("new-secret");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &next, "g2", now)
                .await
                .unwrap();
        let signed = proof(&identity, prepared.statement(), now).await;
        prepared
            .publish(&store, &signed, now, "cfg2")
            .await
            .unwrap();
        for provider in [PaidProvider::Surplus, PaidProvider::Metered] {
            assert!(
                store
                    .read(&keys::paid_control_observation(provider))
                    .await
                    .unwrap()
                    .is_none()
            );
        }
        assert!(
            PreparedConfigurationPublication::capture(&store, &identity, &first, "g2", now)
                .await
                .is_err()
        );
    });
}

#[test]
fn capability_payload_and_current_owner_custody_cannot_be_forged() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        let now = 1_792_339_000_000;
        bootstrap(&store, &identity, now).await;
        let input = inputs("first-secret");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &input, "g1", now)
                .await
                .unwrap();
        let wrong_body = proof(&identity, b"{}", now).await;
        assert_eq!(
            prepared
                .publish(&store, &wrong_body, now, "unused")
                .await
                .unwrap_err()
                .status,
            403
        );
        assert!(
            store
                .read(&keys::paid_control_config())
                .await
                .unwrap()
                .is_none()
        );
        let read_only = make_identity(NativeCapability::SupervisorRead);
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &input, "g1", now)
                .await
                .unwrap();
        let signed = proof(&read_only, prepared.statement(), now).await;
        assert_eq!(
            prepared
                .publish(&store, &signed, now, "unused")
                .await
                .unwrap_err()
                .status,
            403
        );
        let forged = NativeTransportIdentity::new(
            "fixture-key".into(),
            "fixture-owner".into(),
            "fixture-epoch".into(),
            [12; 32],
            BTreeSet::from([NativeCapability::ConfigurationPublish]),
        )
        .unwrap();
        assert!(
            PreparedConfigurationPublication::capture(&store, &forged, &input, "g1", now)
                .await
                .is_err()
        );
        let owner: PaidControlOwner = store::decode(
            store
                .read(&keys::paid_control_owner())
                .await
                .unwrap()
                .as_ref(),
            now,
        )
        .unwrap()
        .unwrap();
        assert_eq!(owner.key_fingerprint, identity.key_fingerprint());
    });
}

#[test]
fn missing_live_owner_never_bootstraps_as_a_side_effect() {
    run(async {
        let store = Memory::default();
        let identity = NativeTransportIdentity::new(
            "live-key".into(),
            "live-owner".into(),
            "live-epoch".into(),
            [12; 32],
            BTreeSet::from([NativeCapability::ConfigurationPublish]),
        )
        .unwrap();
        let now = 1_792_339_000_000;
        let statement = fixture_owner_statement(&identity).unwrap();
        let signed = proof(&identity, &statement, now).await;
        assert!(
            bootstrap_fixture_owner(&store, &identity, &signed, now, "unused")
                .await
                .is_err()
        );
        assert!(
            PreparedConfigurationPublication::capture(
                &store,
                &identity,
                &inputs("first-secret"),
                "g1",
                now
            )
            .await
            .is_err()
        );
        assert!(store.0.lock().unwrap().is_empty());
    });
}

#[test]
fn signed_sender_receiver_configuration_receipt_and_lost_response_retry()
-> Result<(), Box<dyn Error>> {
    run(async {
        let store = Memory::default();
        let identity = publication_identity();
        bootstrap(&store, &identity, now()).await;
        let inputs = inputs("fixture-paid-key");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &inputs, "g1", now())
                .await?;
        let command = gateway_publication::wire::configuration_command(&prepared, "config-r1")?;
        let receiver = PublicationReceiver::configured(identity.clone(), inputs, vec![], now)?;
        let carrier = LocalCarrier {
            receiver: &receiver,
            store: &store,
            lose_response: std::cell::Cell::new(true),
            tamper_response: false,
        };
        let sender = NativePublicationSender::configured(&identity, &carrier);
        let lost = fixture_expect_error(sender.send(&command, [1; 32], now()).await);
        assert_eq!(
            lost.envelope.error.code.as_deref(),
            Some("native_publication_outcome_unknown")
        );
        let current = store
            .read(&keys::paid_control_config())
            .await?
            .ok_or_else(|| std::io::Error::other("fixture configuration row missing"))?;
        assert_eq!(current.revision, "config-r1");
        let receipt = sender.send(&command, [2; 32], now()).await?;
        assert_eq!(receipt.disposition, PublicationDisposition::AlreadyCurrent);
        assert_eq!(receipt.configuration_revision.as_deref(), Some("config-r1"));
        let current = store
            .read(&keys::paid_control_config())
            .await?
            .ok_or_else(|| std::io::Error::other("fixture configuration row missing"))?;
        assert_eq!(current.revision, "config-r1");
        let replay = fixture_expect_error(sender.send(&command, [2; 32], now()).await);
        assert_eq!(
            replay.envelope.error.code.as_deref(),
            Some("native_publication_response_unverified")
        );
        let bad_carrier = LocalCarrier {
            receiver: &receiver,
            store: &store,
            lose_response: std::cell::Cell::new(false),
            tamper_response: true,
        };
        let sender = NativePublicationSender::configured(&identity, &bad_carrier);
        let tampered = fixture_expect_error(sender.send(&command, [3; 32], now()).await);
        assert_eq!(
            tampered.envelope.error.code.as_deref(),
            Some("native_publication_response_unverified")
        );
        Ok::<(), Box<dyn Error>>(())
    })
}

#[test]
fn configuration_capture_retains_frozen_constructor_bytes_without_writes() {
    run(async {
        let store = Memory::default();
        let identity = make_identity(NativeCapability::ConfigurationPublish);
        bootstrap(&store, &identity, now()).await;
        set(
            &store,
            runtime_config_key(),
            "runtime-r1",
            json!({"model":"fixture-model","reasoning":"none"}),
        );
        let before = serde_json::to_vec(&*store.0.lock().unwrap()).unwrap();
        let inputs = inputs("fixture-surplus-key");
        let captured = PreparedConfigurationPublication::capture(
            &store,
            &identity,
            &inputs,
            "generation-1",
            now(),
        )
        .await
        .unwrap();
        // Frozen pre-factor constructor and statement field order, including
        // serde_json preserve_order, independent role bindings and source check.
        let expected = r##"{"operation":"configuration_publish","config":{"v":2,"owner_id":"fixture-owner","owner_epoch":"fixture-epoch","generation":"generation-1","configuration_fingerprint":"7e4f95cb2d3e6fc83456a70ee81483136e0a7df605d26c54e35ad2adbee4877a","surplus_binding":"73ca14b414d233bf44ab369bebeb5738a0b40225cf785b51bfc3598592da17f2","openlux_discovery_binding":"0659c996bc5d3905c397ebf64917f0676f4bd6256640548758cce1c3d57dc524","openlux_binding":"0659c996bc5d3905c397ebf64917f0676f4bd6256640548758cce1c3d57dc524","source_checks":[{"key":"tuple-v1:02756f735f6169000272756e74696d655f636f6e6669670002763200","expected_revision":"runtime-r1"}]},"owner_revision":"owner-r1","previous_config_revision":null}"##;
        assert_eq!(captured.statement(), expected.as_bytes());
        assert_eq!(
            serde_json::to_vec(&*store.0.lock().unwrap()).unwrap(),
            before
        );
        set(
            &store,
            keys::paid_control_config(),
            "config-r1",
            serde_json::to_value(captured.configuration()).unwrap(),
        );
        set(
            &store,
            runtime_config_key(),
            "runtime-r2",
            json!({"model":"fixture-model","reasoning":"none"}),
        );
        assert_eq!(
            PreparedConfigurationPublication::capture(
                &store,
                &identity,
                &inputs,
                "generation-1",
                now()
            )
            .await
            .err()
            .unwrap()
            .status,
            409
        );
        let next = PreparedConfigurationPublication::capture(
            &store,
            &identity,
            &inputs,
            "generation-2",
            now(),
        )
        .await
        .unwrap();
        assert_eq!(
            next.configuration().configuration_fingerprint,
            captured.configuration().configuration_fingerprint
        );
        assert_eq!(
            next.configuration().source_checks[0]
                .expected_revision
                .as_deref(),
            Some("runtime-r2")
        );
    });
}

#[test]
fn source_utf16_runtime_capture_and_signed_receiver_keep_the_actual_codec()
-> Result<(), Box<dyn Error>> {
    use gateway_core::{source_control::encode_source_control_record, source_json::SourceJson};
    run(async {
        let store = Memory::default();
        let identity = publication_identity();
        bootstrap(&store, &identity, now()).await;
        let runtime = encode_source_control_record(
            "runtime-profile-r1",
            &SourceJson::parse(r#"{"model":"\ud800","reasoning":"none"}"#)?,
            None,
        )?;
        {
            let mut rows = store
                .0
                .lock()
                .map_err(|_| std::io::Error::other("fixture memory lock poisoned"))?;
            rows.insert(runtime_config_key().0.clone(), runtime.clone());
        }
        let configured = inputs("fixture-paid-key");
        let prepared =
            PreparedConfigurationPublication::capture(&store, &identity, &configured, "g1", now())
                .await?;
        let expected = prepared.configuration().clone();
        assert_eq!(
            expected.source_checks[0].expected_revision.as_deref(),
            Some("runtime-profile-r1")
        );
        let command =
            gateway_publication::wire::configuration_command(&prepared, "profile-config-r1")?;
        let receiver = PublicationReceiver::configured(identity.clone(), configured, vec![], now)?;
        let carrier = LocalCarrier {
            receiver: &receiver,
            store: &store,
            lose_response: std::cell::Cell::new(false),
            tamper_response: false,
        };
        let sender = NativePublicationSender::configured(&identity, &carrier);
        let receipt = sender.send(&command, [71; 32], now()).await?;
        assert_eq!(receipt.disposition, PublicationDisposition::Applied);
        assert_eq!(
            receipt.configuration_fingerprint.as_deref(),
            Some(expected.configuration_fingerprint.as_str())
        );
        let replay = sender.send(&command, [72; 32], now()).await?;
        assert_eq!(replay.disposition, PublicationDisposition::AlreadyCurrent);
        let rows = store
            .0
            .lock()
            .map_err(|_| std::io::Error::other("fixture memory lock poisoned"))?;
        let current = rows
            .get(&runtime_config_key().0)
            .ok_or_else(|| std::io::Error::other("fixture runtime row missing"))?;
        assert_eq!(current, &runtime);
        Ok::<(), Box<dyn Error>>(())
    })
}
