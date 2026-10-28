use gateway_auth_credentials::native_credentials::NativeOwnerId;
use gateway_auth_credentials::{
    provider_credentials::codex_source_pool_key, provider_credentials::read_codex_source_pool,
};
use gateway_auth_native_proof::native_auth::NativeCapability;
use gateway_auth_primitives::crypto::base64url;
use gateway_authority_keys as keys;
use gateway_core::state::AuthorityStore;
use gateway_publication::access_publication::{PreparedAccessPublication, RegisteredAccessOwner};
use gateway_publication::sender::NativePublicationSender;
use gateway_publication::wire::PublicationDisposition;
use gateway_publication_test_support::fixture::{
    AccessReadFixture as AccessRead, FixtureCarrier as LocalCarrier, FixtureMemory as Memory,
    fixture_access as access, fixture_bootstrap as bootstrap, fixture_identity as make_identity,
    fixture_inputs as inputs, fixture_now as now, fixture_proof as proof,
    fixture_publication_identity as publication_identity, fixture_run as run,
    fixture_seed_access as seed_access, fixture_set as set,
};
use gateway_publication_transport::transport::PublicationReceiver;

#[test]
fn access_only_read_publishes_raw_and_normalized_identity_atomically() {
    run(async {
        let store = Memory::default();
        let now = 1_792_339_000_000;
        bootstrap(
            &store,
            &make_identity(NativeCapability::ConfigurationPublish),
            now,
        )
        .await;
        let identity = make_identity(NativeCapability::AccessPublish);
        let old = access(1_792_341_000, "user");
        seed_access(&store, &old);
        let original = read_codex_source_pool(&store, now).await.unwrap().unwrap();
        let next = access(1_792_342_000, "user");
        let registered = RegisteredAccessOwner::configured(
            NativeOwnerId("registered-owner".into()),
            "/tmp/uos-native-fixture".into(),
        )
        .unwrap();
        let prepared = PreparedAccessPublication::capture(
            &store,
            &AccessRead(next.clone()),
            &identity,
            &registered,
            now,
            "raw-r2",
        )
        .await
        .unwrap();
        assert!(
            !String::from_utf8_lossy(prepared.statement()).contains(next.access_token.expose())
        );
        let signed = proof(&identity, prepared.statement(), now).await;
        let normalized = prepared.publish(&store, &signed, now).await.unwrap();
        let source = read_codex_source_pool(&store, now).await.unwrap().unwrap();
        assert_eq!(source.revision, "raw-r2");
        assert_eq!(normalized.pool_versionstamp.0, source.revision);
        assert_eq!(source.pool.accounts[0].access_token, next.access_token);
        assert_eq!(source.pool.accounts[1], original.pool.accounts[1]);
        assert_eq!(
            normalized.accounts[0].credential_version,
            source.pool.accounts[0].identity().credential_version
        );
        assert_eq!(
            normalized.accounts[1].admin_account_hash,
            source.pool.accounts[1]
                .identity()
                .administrative_account_hash
        );
    });
}

#[test]
fn same_identity_access_read_preserves_existing_pool_revision() {
    run(async {
        let store = Memory::default();
        let now = 1_792_339_000_000;
        bootstrap(
            &store,
            &make_identity(NativeCapability::ConfigurationPublish),
            now,
        )
        .await;
        let identity = make_identity(NativeCapability::AccessPublish);
        let old = access(1_792_341_000, "user");
        seed_access(&store, &old);
        let registered = RegisteredAccessOwner::configured(
            NativeOwnerId("registered-owner".into()),
            "/tmp/uos-native-fixture".into(),
        )
        .unwrap();
        let prepared = PreparedAccessPublication::capture(
            &store,
            &AccessRead(old),
            &identity,
            &registered,
            now,
            "unused",
        )
        .await
        .unwrap();
        let signed = proof(&identity, prepared.statement(), now).await;
        let pool = prepared.publish(&store, &signed, now).await.unwrap();
        assert_eq!(pool.pool_versionstamp.0, "raw-r1");
        assert_eq!(
            store
                .read(&codex_source_pool_key())
                .await
                .unwrap()
                .unwrap()
                .revision,
            "raw-r1"
        );
        assert_eq!(
            store.read(&keys::pool()).await.unwrap().unwrap().revision,
            "normalized-r1"
        );
    });
}

#[test]
fn access_publication_refuses_stale_source_wrong_home_and_workspace_identity() {
    run(async {
        let store = Memory::default();
        let now = 1_792_339_000_000;
        bootstrap(
            &store,
            &make_identity(NativeCapability::ConfigurationPublish),
            now,
        )
        .await;
        let identity = make_identity(NativeCapability::AccessPublish);
        let old = access(1_792_341_000, "user");
        seed_access(&store, &old);
        let registered = RegisteredAccessOwner::configured(
            NativeOwnerId("registered-owner".into()),
            "/tmp/uos-native-fixture".into(),
        )
        .unwrap();
        let next = AccessRead(access(1_792_342_000, "user"));
        let prepared = PreparedAccessPublication::capture(
            &store,
            &next,
            &identity,
            &registered,
            now,
            "raw-r2",
        )
        .await
        .unwrap();
        let signed = proof(&identity, prepared.statement(), now).await;
        let mut changed = store.read(&codex_source_pool_key()).await.unwrap().unwrap();
        changed.revision = "raced-source".into();
        set(
            &store,
            codex_source_pool_key(),
            "raced-source",
            changed.value,
        );
        assert_eq!(
            prepared
                .publish(&store, &signed, now)
                .await
                .unwrap_err()
                .status,
            409
        );
        seed_access(&store, &old);
        let wrong = RegisteredAccessOwner::configured(
            NativeOwnerId("registered-owner".into()),
            "/tmp/wrong-owner".into(),
        )
        .unwrap();
        assert!(
            PreparedAccessPublication::capture(&store, &next, &identity, &wrong, now, "unused")
                .await
                .is_err()
        );
        assert!(
            PreparedAccessPublication::capture(
                &store,
                &AccessRead(access(1_792_342_000, "other-user")),
                &identity,
                &registered,
                now,
                "unused"
            )
            .await
            .is_err()
        );
        assert_eq!(
            store
                .read(&codex_source_pool_key())
                .await
                .unwrap()
                .unwrap()
                .revision,
            "raw-r1"
        );
    });
}

#[test]
fn signed_access_transport_retry_preserves_source_pool_and_other_account() {
    run(async {
        let store = Memory::default();
        let identity = publication_identity();
        bootstrap(&store, &identity, now()).await;
        let old = access(1_792_341_000, "user");
        seed_access(&store, &old);
        let original = read_codex_source_pool(&store, now())
            .await
            .unwrap()
            .unwrap();
        let next = AccessRead(access(1_792_342_000, "user"));
        let registered = RegisteredAccessOwner::configured(
            NativeOwnerId("registered-owner".into()),
            "/tmp/uos-native-fixture".into(),
        )
        .unwrap();
        let prepared = PreparedAccessPublication::capture(
            &store,
            &next,
            &identity,
            &registered,
            now(),
            "raw-r2",
        )
        .await
        .unwrap();
        let command = prepared.command().unwrap();
        let receiver = PublicationReceiver::configured(
            identity.clone(),
            inputs("fixture-paid-key"),
            vec![registered],
            now,
        )
        .unwrap();
        let carrier = LocalCarrier {
            receiver: &receiver,
            store: &store,
            lose_response: std::cell::Cell::new(true),
            tamper_response: false,
        };
        let sender = NativePublicationSender::configured(&identity, &carrier);
        assert!(sender.send(&command, [4; 32], now()).await.is_err());
        let receipt = sender.send(&command, [5; 32], now()).await.unwrap();
        assert_eq!(receipt.disposition, PublicationDisposition::AlreadyCurrent);
        assert_eq!(receipt.source_revision.as_deref(), Some("raw-r2"));
        assert_eq!(receipt.pool_versionstamp, receipt.source_revision);
        let current = read_codex_source_pool(&store, now())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(current.pool.accounts[1], original.pool.accounts[1]);
        assert_eq!(current.revision, "raw-r2");
    });
}

#[test]
fn native_publication_headers_preserve_duplicate_and_missing_auth_rejection() {
    use gateway_publication_policy::protocol::OwnedNativeAuthHeaders;
    assert!(OwnedNativeAuthHeaders::from_pairs(Vec::new()).is_err());
    let values = vec![
        ("x-uos-native-version".into(), "1".into()),
        ("x-uos-native-key-id".into(), "fixture-key".into()),
        ("x-uos-native-time-ms".into(), now().to_string()),
        ("x-uos-native-nonce".into(), base64url(&[1; 32])),
        ("x-uos-native-mac".into(), base64url(&[2; 32])),
    ];
    assert!(OwnedNativeAuthHeaders::from_pairs(values.clone()).is_ok());
    let mut duplicated = values;
    duplicated.push(("X-UOS-NATIVE-MAC".into(), base64url(&[3; 32])));
    assert!(OwnedNativeAuthHeaders::from_pairs(duplicated).is_err());
}
