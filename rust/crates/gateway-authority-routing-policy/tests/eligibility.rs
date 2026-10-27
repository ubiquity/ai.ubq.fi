use gateway_authority_contracts::DAY_MS;
use gateway_authority_routing_evidence::{
    capacity::{
        CapacityObservation, CapacityState, CapacityStore, CapacityWindow, CapacityWindows,
    },
    models::{AccountCatalog, ModelAvailability, UnsupportedModel, model_quota_class},
};
use gateway_authority_routing_policy::{
    RoutingPolicyData, cohort_matches, evaluate, unavailable_model,
};
use gateway_authority_routing_records::{
    codec::normalize,
    types::{
        PoolAccount, ProbeCircuit, ProbeLease, QuotaBlock, QuotaBlockSource, RoutingState,
        SubscriptionPool,
    },
};
use gateway_core::{
    identity::ActiveSelection, identity::ActiveTransitionReason, identity::CredentialVersion,
    identity::PoolVersionstamp, identity::RoutingAccountHash, identity::SubscriptionSelectionHash,
    model::ModelId, provider::CodexQuotaClass,
};
use gateway_source::{error::GatewayError, js_string::JsString16};
use std::collections::BTreeMap;

const NOW: i64 = 1_000_000_000;
const CLASS: CodexQuotaClass = CodexQuotaClass::Standard;
type TestResult = Result<(), GatewayError>;
struct Inputs {
    pool: SubscriptionPool,
    state: RoutingState,
    active: Option<ActiveSelection>,
    models: ModelAvailability,
    capacity: CapacityStore,
    overage: BTreeMap<String, bool>,
}
impl Inputs {
    fn new() -> Result<Self, GatewayError> {
        let pool = SubscriptionPool {
            v: 1,
            pool_versionstamp: PoolVersionstamp("pool".into()),
            accounts: ["a", "b"]
                .into_iter()
                .map(|id| PoolAccount {
                    account_id_hash: RoutingAccountHash(id.into()),
                    admin_account_hash: SubscriptionSelectionHash(format!("admin-{id}")),
                    credential_version: CredentialVersion(format!("credential-{id}")),
                    enabled: true,
                })
                .collect(),
        };
        let state = normalize(&pool, None, NOW)?;
        Ok(Self {
            pool,
            state,
            active: None,
            models: ModelAvailability::default(),
            capacity: CapacityStore::default(),
            overage: BTreeMap::new(),
        })
    }
    fn view(&self) -> RoutingPolicyData<'_> {
        RoutingPolicyData {
            pool: &self.pool,
            state: &self.state,
            active: self.active.as_ref(),
            models: &self.models,
            capacity: &self.capacity,
            overage: &self.overage,
        }
    }
    fn sample(&mut self, used: f64, reset: Option<i64>) {
        self.capacity.observations = vec![CapacityObservation {
            slot: 0,
            account_id_hash: self.pool.accounts[0].account_id_hash.clone(),
            state: CapacityState::Available,
            source_observed_at_ms: None,
            snapshot_at_ms: NOW - 10,
            windows: CapacityWindows {
                primary: Some(CapacityWindow {
                    limit_window_seconds: None,
                    used_percent: Some(used),
                    reset_at_ms: reset,
                }),
                secondary: None,
            },
            additional_rate_limits: vec![],
        }];
    }
}
fn block(observed: i64, until: i64) -> QuotaBlock {
    QuotaBlock {
        blocked_until_ms: until,
        source: QuotaBlockSource::BodyResetsAt,
        legacy_fallback: false,
        observed_at_ms: Some(observed),
        reset_at_ms: Some(until),
        reset_is_stable: true,
        reset_ambiguous: false,
        recovery_pending: false,
    }
}

#[test]
fn newer_positive_capacity_overrides_old_block_but_never_a_newer_live_signal() -> TestResult {
    let mut inputs = Inputs::new()?;
    inputs.state.slots[0].insert_quota_block(CLASS, block(NOW - 20, NOW + 100));
    inputs.sample(25.0, Some(NOW + 200));
    let positive = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
    assert!(positive.eligible);
    assert!(!positive.probe);
    inputs.state.slots[0].insert_quota_block(CLASS, block(NOW - 10, NOW + 100));
    let newer = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
    assert!(!newer.eligible);
    assert_eq!(newer.reason, Some(ActiveTransitionReason::QuotaExhausted));
    assert_eq!(newer.retry, Some(NOW + 100));
    Ok(())
}

#[test]
fn deadline_less_exhaustion_and_enabled_overage_are_probe_data() -> TestResult {
    let mut inputs = Inputs::new()?;
    for reset in [None, Some(NOW), Some(NOW + 200)] {
        inputs.sample(100.0, reset);
        let row = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
        assert_eq!(row.eligible, reset.is_none_or(|at| at <= NOW));
        assert_eq!(row.probe, row.eligible);
    }
    inputs.overage.insert("a".into(), true);
    let row = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
    assert!(row.eligible && row.probe);
    assert_eq!(row.reason, None);
    Ok(())
}

#[test]
fn quota_class_and_foreign_probe_circuit_stay_separate() -> TestResult {
    let mut inputs = Inputs::new()?;
    inputs.state.slots[0].insert_quota_block(CodexQuotaClass::Spark, block(NOW - 10, NOW + 200));
    assert!(evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW).eligible);
    assert!(
        !evaluate(
            &inputs.view(),
            0,
            &ModelId::from("gpt-5.3-codex-spark"),
            CodexQuotaClass::Spark,
            NOW
        )
        .eligible
    );
    inputs.state.slots[0].probe_lease = Some(ProbeLease {
        token: "probe".into(),
        generation: 0,
        circuit: ProbeCircuit::Quota,
        quota_class: Some(CLASS),
        expires_at_ms: NOW + 100,
    });
    let held = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
    assert!(!held.eligible);
    assert_eq!(held.reason, None);
    if let Some(lease) = inputs.state.slots[0].probe_lease.as_mut() {
        lease.circuit = ProbeCircuit::UpstreamTimeout;
    }
    assert!(evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW).eligible);
    if let Some(lease) = inputs.state.slots[0].probe_lease.as_mut() {
        lease.circuit = ProbeCircuit::Quota;
        lease.quota_class = Some(CodexQuotaClass::Reserve);
    }
    assert!(evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW).eligible);
    Ok(())
}

#[test]
fn expired_block_requests_probe_but_a_live_class_lease_remains_incomplete() -> TestResult {
    let mut inputs = Inputs::new()?;
    inputs.state.slots[0].insert_quota_block(CLASS, block(NOW - 100, NOW));
    let row = evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW);
    assert!(row.eligible && row.probe);
    assert_eq!(row.reason, None);
    inputs.state.slots[0].probe_lease = Some(ProbeLease {
        token: "probe".into(),
        generation: 0,
        circuit: ProbeCircuit::Quota,
        quota_class: None,
        expires_at_ms: NOW + 1,
    });
    assert!(!evaluate(&inputs.view(), 0, &ModelId::from("gpt-test"), CLASS, NOW).eligible);
    assert!(
        evaluate(
            &inputs.view(),
            0,
            &ModelId::from("gpt-test"),
            CLASS,
            NOW + 1
        )
        .eligible
    );
    Ok(())
}

#[test]
fn learned_model_time_bounds_preserve_utf16_and_current_invalidity_precedes_it() -> TestResult {
    let mut inputs = Inputs::new()?;
    let model = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    let detail = JsString16::from_units(vec![0xdfff]);
    for observed in [NOW - 7 * DAY_MS - 1, NOW - 7 * DAY_MS, NOW, NOW + 1] {
        inputs.models.unsupported.insert(
            "a".into(),
            BTreeMap::from([(
                model.clone(),
                UnsupportedModel {
                    observed_at_ms: observed,
                    detail: detail.clone(),
                },
            )]),
        );
        let blocked = unavailable_model(
            &inputs.models,
            &inputs.pool,
            &RoutingAccountHash("a".into()),
            &model,
            NOW,
        );
        assert_eq!(
            blocked.is_some(),
            observed > 0 && observed <= NOW && NOW - observed <= 7 * DAY_MS
        );
    }
    if let Some(row) = inputs
        .models
        .unsupported
        .get_mut("a")
        .and_then(|rows| rows.get_mut(&model))
    {
        row.observed_at_ms = NOW;
    }
    inputs.state.slots[0].invalid_credential_version =
        Some(inputs.state.slots[0].credential_version.clone());
    assert_eq!(
        evaluate(&inputs.view(), 0, &model, CLASS, NOW).reason,
        Some(ActiveTransitionReason::CredentialInvalid)
    );
    Ok(())
}

#[test]
fn sibling_catalog_proof_uses_exact_model_units_version_and_enabled_membership() -> TestResult {
    let mut inputs = Inputs::new()?;
    let lone = ModelId::from_source(JsString16::from_units(vec![0xd800]));
    inputs.models.catalogs.insert(
        "a".into(),
        AccountCatalog {
            client_version: "1.0.0".into(),
            models: vec![],
        },
    );
    inputs.models.catalogs.insert(
        "b".into(),
        AccountCatalog {
            client_version: "1.0.0".into(),
            models: vec![lone.clone()],
        },
    );
    assert_eq!(
        unavailable_model(
            &inputs.models,
            &inputs.pool,
            &RoutingAccountHash("a".into()),
            &lone,
            NOW
        ),
        Some(None)
    );
    assert_eq!(
        unavailable_model(
            &inputs.models,
            &inputs.pool,
            &RoutingAccountHash("a".into()),
            &ModelId::from("�"),
            NOW
        ),
        None
    );
    inputs.pool.accounts[1].enabled = false;
    assert_eq!(
        unavailable_model(
            &inputs.models,
            &inputs.pool,
            &RoutingAccountHash("a".into()),
            &lone,
            NOW
        ),
        None
    );
    inputs.pool.accounts[1].enabled = true;
    if let Some(catalog) = inputs.models.catalogs.get_mut("b") {
        catalog.client_version = "2.0.0".into();
    }
    assert_eq!(
        unavailable_model(
            &inputs.models,
            &inputs.pool,
            &RoutingAccountHash("a".into()),
            &lone,
            NOW
        ),
        None
    );
    Ok(())
}

#[test]
fn bindings_and_cohort_predicates_are_data_and_refuse_partial_or_reordered_cohorts() -> TestResult {
    let mut inputs = Inputs::new()?;
    for slot in &mut inputs.state.slots {
        slot.insert_quota_block(CLASS, block(NOW - 1, NOW + 200));
    }
    let bindings = [inputs.view().binding(0), inputs.view().binding(1)];
    assert!(cohort_matches(
        &inputs.view(),
        &bindings,
        &ModelId::from("gpt-test"),
        CLASS,
        true,
        NOW
    ));
    assert!(!cohort_matches(
        &inputs.view(),
        &bindings[..1],
        &ModelId::from("gpt-test"),
        CLASS,
        true,
        NOW
    ));
    assert!(!cohort_matches(
        &inputs.view(),
        &[bindings[1].clone(), bindings[0].clone()],
        &ModelId::from("gpt-test"),
        CLASS,
        true,
        NOW
    ));
    inputs
        .pool
        .accounts
        .iter_mut()
        .for_each(|account| account.enabled = false);
    assert!(!cohort_matches(
        &inputs.view(),
        &[],
        &ModelId::from("gpt-test"),
        CLASS,
        false,
        NOW
    ));
    assert!(!inputs.view().matches_admission(&bindings[0], 1));
    Ok(())
}

#[test]
fn defining_model_helper_keeps_source_trim_and_explicit_class_rules() {
    for (units, expected) in [
        (
            JsString16::from_utf8("\u{feff} GPT-5.3-CODEX-SPARK \u{feff}"),
            CodexQuotaClass::Spark,
        ),
        (
            JsString16::from_utf8("GPT-RESERVE"),
            CodexQuotaClass::Reserve,
        ),
        (
            JsString16::from_utf8("gpt-oss-120b"),
            CodexQuotaClass::GptOss120b,
        ),
        (JsString16::from_units(vec![0xd800]), CLASS),
        (JsString16::from_utf8("\u{feff} "), CodexQuotaClass::Unknown),
    ] {
        assert_eq!(model_quota_class(&ModelId::from_source(units)), expected);
    }
}
