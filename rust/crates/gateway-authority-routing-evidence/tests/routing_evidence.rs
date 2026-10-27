use gateway_auth::provider_credentials::{codex_source_pool_key, parse_codex_source_pool};
use gateway_authority_contracts::invalid;
use gateway_authority_keys as keys;
use gateway_authority_routing_evidence::{
    capacity::CapacityState,
    reader::{RoutingEvidence, RoutingEvidenceReader, RoutingReadMode},
};
use gateway_authority_routing_records::types::{PoolAccount, SubscriptionPool};
use gateway_core::{
    identity::AccountBinding, identity::ActiveSelection, identity::MAX_SAFE_INTEGER,
    identity::PoolVersionstamp, model::ModelId, source_control::encode_source_control_record,
    state::AtomicTransition, state::AuthorityStore, state::CommitOutcome, state::RecordKey,
    state::RevisionCheck, state::StoredRecord, value::PortableValue, value::ValueEncoding,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde_json::{Value, json};
use std::{
    cell::RefCell,
    collections::{BTreeMap, BTreeSet},
    future::Future,
    task::{Context, Poll, Waker},
};

const NOW: i64 = 1_000;

#[derive(Default)]
struct ObservedStore {
    rows: BTreeMap<String, StoredRecord>,
    reads: RefCell<Vec<RecordKey>>,
    failure: Option<String>,
}

impl AuthorityStore for ObservedStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        self.reads.borrow_mut().push(key.clone());
        if self.failure.as_deref() == Some(key.0.as_str()) {
            return Err(GatewayError::unavailable(
                "synthetic read failed",
                "synthetic_backend",
            ));
        }
        Ok(self.rows.get(&key.0).cloned())
    }

    async fn commit(&self, _transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        panic!("routing evidence must never commit, even to verify currentness")
    }
}

fn ready<F: Future>(future: F) -> F::Output {
    let mut future = std::pin::pin!(future);
    match future
        .as_mut()
        .poll(&mut Context::from_waker(Waker::noop()))
    {
        Poll::Ready(value) => value,
        Poll::Pending => panic!("synthetic store unexpectedly yielded"),
    }
}

struct Fixture {
    store: ObservedStore,
    pool: SubscriptionPool,
}

impl Fixture {
    fn new() -> Result<Self, GatewayError> {
        let raw = json!({"accounts":[
            {"account_id":" raw α ","access_token":"synthetic-access-a",
             "refresh_token":"synthetic-refresh-a","updated_at_ms":1},
            {"account_id":"raw-b","access_token":"synthetic-access-b",
             "refresh_token":"synthetic-refresh-b","updated_at_ms":2}
        ],"updated_at_ms":2});
        let source = parse_codex_source_pool(&raw)?
            .ok_or_else(|| invalid("synthetic fixture pool is malformed"))?;
        let pool = SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("source-r1".into()),
            accounts: source
                .accounts
                .iter()
                .map(|account| {
                    let identity = account.identity();
                    PoolAccount {
                        account_id_hash: identity.routing_account_hash,
                        admin_account_hash: identity.administrative_account_hash,
                        credential_version: identity.credential_version,
                        enabled: false,
                    }
                })
                .collect(),
        };
        let mut store = ObservedStore::default();
        store.rows.insert(
            codex_source_pool_key().0,
            StoredRecord::json("source-r1", raw, None),
        );
        store.rows.insert(
            keys::pool().0,
            StoredRecord::json(
                "pool-r1",
                serde_json::to_value(&pool).map_err(|_| invalid("synthetic pool encoding"))?,
                None,
            ),
        );
        Ok(Self { store, pool })
    }

    fn read(&self, mode: RoutingReadMode) -> Result<RoutingEvidence, GatewayError> {
        ready(RoutingEvidenceReader::new(&self.store, NOW)?.read(mode))
    }

    fn error(&self, mode: RoutingReadMode) -> GatewayError {
        match self.read(mode) {
            Err(error) => error,
            Ok(_) => panic!("malformed synthetic state unexpectedly hydrated"),
        }
    }

    fn row_mut(&mut self, key: RecordKey) -> &mut StoredRecord {
        self.store
            .rows
            .entry(key.0)
            .or_insert_with(|| StoredRecord::json("row-r1", Value::Null, None))
    }

    fn replace_pool(&mut self) -> Result<(), GatewayError> {
        let value =
            serde_json::to_value(&self.pool).map_err(|_| invalid("synthetic pool encoding"))?;
        self.row_mut(keys::pool()).value = value;
        Ok(())
    }
}

fn fence<'a>(evidence: &'a RoutingEvidence, key: &RecordKey) -> &'a RevisionCheck {
    evidence
        .revision_checks()
        .iter()
        .find(|check| check.key == *key)
        .unwrap_or_else(|| panic!("missing expected fixed-family fence"))
}

fn assert_error(error: &GatewayError, code: &str, message: &str) {
    assert_eq!(error.status, 503);
    assert_eq!(error.envelope.error.error_type, "server_error");
    assert_eq!(error.envelope.error.code.as_deref(), Some(code));
    assert_eq!(error.envelope.error.message, message);
}

#[test]
fn fixed_readset_preserves_present_absent_and_expired_fences() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    *fixture.row_mut(keys::capacity()) =
        StoredRecord::json("expired-capacity", json!("bad body"), Some(NOW));
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert_eq!(evidence.mode(), RoutingReadMode::GlobalSelection);
    assert_eq!(evidence.observed_at_ms(), NOW);
    assert_eq!(
        fence(&evidence, &keys::pool()).expected_revision.as_deref(),
        Some("pool-r1")
    );
    assert_eq!(fence(&evidence, &keys::routing()).expected_revision, None);
    assert_eq!(
        fence(&evidence, &keys::capacity())
            .expected_revision
            .as_deref(),
        Some("expired-capacity")
    );
    assert_eq!(evidence.capacity().v, 1);
    assert!(evidence.capacity().observations.is_empty());
    assert!(
        evidence
            .pool()
            .accounts
            .iter()
            .all(|account| account.enabled)
    );
    assert_eq!(evidence.state().slots.len(), 2);
    let mut expected = vec![
        keys::pool(),
        keys::routing(),
        keys::active(),
        keys::capacity(),
        codex_source_pool_key(),
        keys::models(),
        keys::provider_selection(),
    ];
    expected.extend(
        fixture
            .pool
            .accounts
            .iter()
            .map(|account| keys::overage(&account.admin_account_hash.0)),
    );
    assert_eq!(*fixture.store.reads.borrow(), expected);
    let read_keys: BTreeSet<_> = expected.iter().map(|key| key.0.as_str()).collect();
    assert_eq!(read_keys.len(), expected.len());
    assert_eq!(evidence.revision_checks().len(), expected.len());
    let fenced_keys: BTreeSet<_> = evidence
        .revision_checks()
        .iter()
        .map(|check| check.key.0.as_str())
        .collect();
    assert_eq!(fenced_keys, read_keys);
    assert!(
        evidence
            .revision_checks()
            .windows(2)
            .all(|pair| pair[0].key.0 < pair[1].key.0)
    );
    Ok(())
}

#[test]
fn pinned_mode_never_observes_or_fences_global_active() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(keys::active()).schema_version = 99;
    let evidence = fixture.read(RoutingReadMode::PinnedObservation)?;
    assert_eq!(evidence.mode(), RoutingReadMode::PinnedObservation);
    assert!(evidence.active().is_none());
    assert!(
        !evidence
            .revision_checks()
            .iter()
            .any(|check| check.key == keys::active())
    );
    assert!(!fixture.store.reads.borrow().contains(&keys::active()));
    assert_eq!(evidence.revision_checks().len(), 8);
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_version_unsupported",
        "The authority state version is unsupported.",
    );
    Ok(())
}

#[test]
fn global_active_keeps_original_identity_and_generation() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    let account = &fixture.pool.accounts[0];
    let active = ActiveSelection {
        v: 1,
        binding: AccountBinding {
            account_id_hash: account.account_id_hash.clone(),
            credential_version: account.credential_version.clone(),
            pool_versionstamp: fixture.pool.pool_versionstamp.clone(),
            slot: 0,
            routing_generation: 0,
        },
        generation: 7,
        transition_reason: None,
        updated_at_ms: NOW,
    };
    fixture.row_mut(keys::active()).value =
        serde_json::to_value(&active).map_err(|_| invalid("synthetic active encoding"))?;
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert_eq!(evidence.active(), Some(&active));
    fixture.row_mut(keys::active()).value["generation"] = json!(0);
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Malformed active Codex selection.",
    );
    Ok(())
}

#[test]
fn metadata_validation_precedes_expiry_and_payload_decode() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    let key = keys::capacity();
    let row = fixture.row_mut(key.clone());
    row.schema_version = 99;
    row.revision = " ".into();
    row.expires_at_ms = Some(-1);
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_version_unsupported",
        "The authority state version is unsupported.",
    );
    fixture.row_mut(key.clone()).schema_version = 1;
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Missing or oversized authority identity.",
    );
    fixture.row_mut(key.clone()).revision = "valid-revision".into();
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Expected a nonnegative safe integer.",
    );
    fixture.row_mut(key).expires_at_ms = Some(NOW);
    assert!(
        fixture
            .read(RoutingReadMode::GlobalSelection)?
            .capacity()
            .observations
            .is_empty()
    );
    Ok(())
}

#[test]
fn expired_typed_payload_still_requires_valid_physical_encoding() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    let row = fixture.row_mut(keys::models());
    row.expires_at_ms = Some(NOW);
    row.value_encoding = ValueEncoding::ProfileV2;
    row.value = json!("invalid portable value");
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "portable_value_invalid",
        "Typed value decoding failed.",
    );
    Ok(())
}

#[test]
fn only_models_use_the_named_source_control_codec() -> Result<(), GatewayError> {
    let fixture = Fixture::new()?;
    let overage = keys::overage(&fixture.pool.accounts[0].admin_account_hash.0);
    for key in [
        keys::pool(),
        keys::routing(),
        keys::active(),
        keys::capacity(),
        codex_source_pool_key(),
        keys::provider_selection(),
        overage,
    ] {
        let mut fixture = Fixture::new()?;
        fixture.store.rows.insert(
            key.0,
            StoredRecord::profile_v2("source-r1", PortableValue::Null, None)?,
        );
        assert_error(
            &fixture.error(RoutingReadMode::GlobalSelection),
            "incompatible_value_encoding",
            "A JSON record was required.",
        );
    }
    Ok(())
}

#[test]
fn expired_active_and_models_do_not_decode_logical_body() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    for key in [keys::active(), keys::models(), keys::provider_selection()] {
        *fixture.row_mut(key) = StoredRecord::json("expired-body", json!([false]), Some(NOW - 1));
    }
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert!(evidence.active().is_none());
    assert!(evidence.models().catalogs.is_empty());
    assert_eq!(
        evidence.models_source().as_str(),
        "{\"accounts\":{},\"unsupported\":{}}"
    );
    assert_eq!(
        fence(&evidence, &keys::models())
            .expected_revision
            .as_deref(),
        Some("expired-body")
    );
    assert!(
        evidence
            .pool()
            .accounts
            .iter()
            .all(|account| account.enabled)
    );
    Ok(())
}

#[test]
fn current_source_revision_is_required_before_models_or_overage() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(codex_source_pool_key()).revision = "source-r2".into();
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "codex_routing_unavailable",
        "Normalized Codex pool is not bound to the current source revision.",
    );
    assert!(!fixture.store.reads.borrow().contains(&keys::models()));
    assert!(
        !fixture
            .store
            .reads
            .borrow()
            .contains(&keys::provider_selection())
    );
    Ok(())
}

#[test]
fn required_pool_and_source_expiry_keep_unavailable_errors() -> Result<(), GatewayError> {
    for expired in [false, true] {
        let mut fixture = Fixture::new()?;
        if expired {
            fixture.row_mut(keys::pool()).expires_at_ms = Some(NOW);
        } else {
            fixture.store.rows.remove(&keys::pool().0);
        }
        assert_error(
            &fixture.error(RoutingReadMode::GlobalSelection),
            "codex_routing_unavailable",
            "Codex pool is unavailable.",
        );
        assert_eq!(*fixture.store.reads.borrow(), vec![keys::pool()]);
        let mut fixture = Fixture::new()?;
        if expired {
            fixture.row_mut(codex_source_pool_key()).expires_at_ms = Some(NOW);
        } else {
            fixture.store.rows.remove(&codex_source_pool_key().0);
        }
        assert_error(
            &fixture.error(RoutingReadMode::GlobalSelection),
            "codex_routing_unavailable",
            "Current raw Codex source pool is unavailable.",
        );
        assert!(!fixture.store.reads.borrow().contains(&keys::models()));
    }
    Ok(())
}

#[test]
fn source_order_and_all_credential_identities_are_bound() -> Result<(), GatewayError> {
    for field in [
        "account_id_hash",
        "admin_account_hash",
        "credential_version",
    ] {
        let mut fixture = Fixture::new()?;
        fixture.row_mut(keys::pool()).value["accounts"][0][field] =
            json!("different-valid-identity");
        assert_error(
            &fixture.error(RoutingReadMode::GlobalSelection),
            "codex_routing_unavailable",
            "Normalized Codex account identity differs from the current raw credential.",
        );
    }
    let mut fixture = Fixture::new()?;
    fixture.pool.accounts.swap(0, 1);
    fixture.replace_pool()?;
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "codex_routing_unavailable",
        "Normalized Codex account identity differs from the current raw credential.",
    );
    Ok(())
}

#[test]
fn source_credential_change_cannot_reuse_normalized_projection() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(codex_source_pool_key()).value["accounts"][0]["access_token"] =
        json!("synthetic-replaced-access");
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "codex_routing_unavailable",
        "Normalized Codex account identity differs from the current raw credential.",
    );
    Ok(())
}

#[test]
fn pool_shape_and_raw_pool_shape_fail_at_original_boundary() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.pool.accounts[1].account_id_hash = fixture.pool.accounts[0].account_id_hash.clone();
    fixture.replace_pool()?;
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Duplicate Codex account identity.",
    );
    assert_eq!(*fixture.store.reads.borrow(), vec![keys::pool()]);
    let mut fixture = Fixture::new()?;
    fixture.row_mut(codex_source_pool_key()).value = json!({"accounts":[],"updated_at_ms":1});
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Current raw Codex source pool is malformed.",
    );
    Ok(())
}

#[test]
fn source_selection_overrides_stale_enabled_flags_and_uses_admin_overage_keys()
-> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    let selected = fixture.pool.accounts[1].admin_account_hash.0.clone();
    fixture.row_mut(keys::provider_selection()).value = json!({
        "provider_ids":[format!("codex:{selected}")],"updated_at_ms":1});
    let overage_key = keys::overage(&fixture.pool.accounts[0].admin_account_hash.0);
    fixture.row_mut(overage_key.clone()).value = json!({"allow":true});
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert!(!evidence.pool().accounts[0].enabled);
    assert!(evidence.pool().accounts[1].enabled);
    assert_eq!(
        evidence
            .overage()
            .get(&fixture.pool.accounts[0].account_id_hash.0),
        Some(&true)
    );
    assert_eq!(
        evidence
            .overage()
            .get(&fixture.pool.accounts[1].account_id_hash.0),
        Some(&false)
    );
    assert!(fixture.store.reads.borrow().contains(&overage_key));
    assert!(
        !fixture
            .store
            .reads
            .borrow()
            .contains(&keys::overage(&fixture.pool.accounts[0].account_id_hash.0))
    );
    Ok(())
}

#[test]
fn malformed_selection_and_overage_keep_original_rules() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(keys::provider_selection()).value =
        json!({"provider_ids":[false],"updated_at_ms":1});
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Current source provider selection is malformed.",
    );
    fixture.row_mut(keys::provider_selection()).value = Value::Null;
    let key = keys::overage(&fixture.pool.accounts[0].admin_account_hash.0);
    fixture.row_mut(key.clone()).value = json!({"allow":"true"});
    assert_eq!(
        fixture
            .read(RoutingReadMode::GlobalSelection)?
            .overage()
            .get(&fixture.pool.accounts[0].account_id_hash.0),
        Some(&false)
    );
    *fixture.row_mut(key.clone()) =
        StoredRecord::json("expired-overage", json!({"allow":true}), Some(NOW));
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert_eq!(
        evidence
            .overage()
            .get(&fixture.pool.accounts[0].account_id_hash.0),
        Some(&false)
    );
    assert_eq!(
        fence(&evidence, &key).expected_revision.as_deref(),
        Some("expired-overage")
    );
    Ok(())
}

#[test]
fn profile_models_preserve_lone_utf16_raw_ids_and_unknown_source_tree() -> Result<(), GatewayError>
{
    let mut fixture = Fixture::new()?;
    let raw = SourceJson::parse(
        r#"{"accounts":{" raw α ":{"client_version":"1.2.3","updated_at_ms":1,"slugs":["gpt-x","\ud800","gpt-x"]}},"unsupported":{" raw α ":{"\ud800":500," ":1,"ignored":0}},"extra":{"\udfff":-0.0}}"#,
    )?;
    let record = encode_source_control_record("models-r1", &raw, None)?;
    assert_eq!(record.value_encoding, ValueEncoding::ProfileV2);
    fixture.store.rows.insert(keys::models().0, record);
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    let hash = &fixture.pool.accounts[0].account_id_hash.0;
    assert_eq!(
        evidence.source_ids().get(hash).map(String::as_str),
        Some(" raw α ")
    );
    let catalog = evidence
        .models()
        .catalogs
        .get(hash)
        .ok_or_else(|| invalid("missing catalog"))?;
    let lone = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    assert_eq!(catalog.client_version, "1.2.3");
    assert_eq!(catalog.models, vec![ModelId::from("gpt-x"), lone.clone()]);
    let unsupported = evidence
        .models()
        .unsupported
        .get(hash)
        .ok_or_else(|| invalid("missing unsupported"))?;
    assert_eq!(unsupported.len(), 1);
    let observation = unsupported
        .get(&lone)
        .ok_or_else(|| invalid("missing lone model"))?;
    assert_eq!(observation.observed_at_ms, 500);
    let mut expected = "The '".encode_utf16().collect::<Vec<_>>();
    expected.push(0xd800);
    expected
        .extend("' model is not supported when using Codex with a ChatGPT account.".encode_utf16());
    assert_eq!(observation.detail.as_units(), expected);
    let tree = gateway_core::source_control::source_json_tree(evidence.models_source())?;
    assert_eq!(tree, gateway_core::source_control::source_json_tree(&raw)?);
    Ok(())
}

#[test]
fn json_models_default_on_logical_shape_without_discarding_raw_tree() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(keys::models()).value = json!({"accounts":false,"extra":[1,2,3]});
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert!(evidence.models().catalogs.is_empty());
    assert!(evidence.models().unsupported.is_empty());
    assert_eq!(
        evidence.models_source().deserialize::<Value>()?,
        json!({"accounts":false,"extra":[1,2,3]})
    );
    *fixture.row_mut(keys::models()) =
        StoredRecord::profile("legacy-profile", PortableValue::Null, None)?;
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "source_control_record_invalid",
        "Source control record is not a finite JSON tree.",
    );
    Ok(())
}

#[test]
fn capacity_source_shape_is_decoded_without_producer_trust() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.row_mut(keys::capacity()).value = json!({"v":1,"updated_at_ms":12,"observations":[{
        "slot":1,"account_id_hash":"source-capacity-identity","state":"available","snapshot_at_ms":10,
        "source_observed_at_ms":-1,"windows":{"primary":{"used_percent":25,"reset_at_ms":90},
            "secondary":{"used_percent":101,"limit_window_seconds":60}},
        "additional_rate_limits":[{"limit_name":" GPT Reserve ","metered_feature":" feature ",
            "windows":{"primary":{"used_percent":75}}},{"limit_name":"discard","metered_feature":false,"windows":{}}]
    }]});
    let evidence = fixture.read(RoutingReadMode::GlobalSelection)?;
    assert_eq!(evidence.capacity().updated_at_ms, 12);
    let row = &evidence.capacity().observations[0];
    assert_eq!(row.state, CapacityState::Available);
    assert_eq!(row.account_id_hash.0, "source-capacity-identity");
    assert_eq!(row.source_observed_at_ms, None);
    assert_eq!(
        row.windows
            .primary
            .as_ref()
            .and_then(|window| window.used_percent),
        Some(25.0)
    );
    assert_eq!(
        row.windows
            .secondary
            .as_ref()
            .and_then(|window| window.used_percent),
        None
    );
    assert_eq!(row.additional_rate_limits.len(), 1);
    assert_eq!(row.additional_rate_limits[0].limit_name, "GPT Reserve");
    assert_eq!(
        row.additional_rate_limits[0].metered_feature.as_deref(),
        Some("feature")
    );
    fixture.row_mut(keys::capacity()).value["observations"][0]["slot"] = json!(2);
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "authority_state_invalid",
        "Malformed capacity slot.",
    );
    Ok(())
}

#[test]
fn backend_error_propagates_and_invalid_clock_reads_nothing() -> Result<(), GatewayError> {
    let mut fixture = Fixture::new()?;
    fixture.store.failure = Some(keys::routing().0);
    assert_error(
        &fixture.error(RoutingReadMode::GlobalSelection),
        "synthetic_backend",
        "synthetic read failed",
    );
    assert_eq!(
        *fixture.store.reads.borrow(),
        vec![keys::pool(), keys::routing()]
    );
    for now in [-1, MAX_SAFE_INTEGER + 1] {
        let store = ObservedStore::default();
        assert!(RoutingEvidenceReader::new(&store, now).is_err());
        assert!(store.reads.borrow().is_empty());
    }
    Ok(())
}
