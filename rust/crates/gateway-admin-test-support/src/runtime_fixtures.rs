//! Composition helpers for the original admin-runtime contract cases.
use crate::{runtime_host::Host, runtime_store::Store};
use gateway_admin_contracts::operations::AdminInput;
use gateway_admin_runtime::{
    AdminRuntime, RuntimeConfig,
    capabilities::{Context, ErrorDiagnostics, PricingSnapshot, RuntimeCapabilities},
};
use gateway_auth::types::{AuthMethod, AuthPrincipal, AuthResponseBody};
use gateway_core::{
    error::GatewayError, js_string::JsString16, model::ModelId,
    source_control::encode_source_control_record, source_json::SourceJson, state::StoredRecord,
};
use gateway_schema::{
    catalog_store::{codex_snapshot_key, runtime_config_key},
    runtime_config::build_runtime_config,
    snapshot::ModelSnapshot,
};
use serde_json::{Value, json};
use std::collections::BTreeMap;

// Assertions inspect the actual JSON wire of either explicit response arm.
pub trait WireBody {
    fn wire_json(&self) -> Value;
}
impl WireBody for AuthResponseBody {
    fn wire_json(&self) -> Value {
        serde_json::from_slice(
            &self
                .to_bytes()
                .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error}")),
        )
        .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error}"))
    }
}

pub struct Capabilities;
impl RuntimeCapabilities for Capabilities {
    fn compare_admin_labels(
        &self,
        left: &JsString16,
        right: &JsString16,
    ) -> Result<std::cmp::Ordering, GatewayError> {
        // These legacy cases contain only empty/singleton lists. Unequal source
        // ordering requires the exact locale fixtures in source_controls.rs.
        if left == right {
            return Ok(std::cmp::Ordering::Equal);
        }
        Err(GatewayError::unavailable(
            "Uncovered legacy fixture comparison",
            "fixture_collation_missing",
        ))
    }
    async fn metered_diagnostics(&self) -> Result<Value, GatewayError> {
        Ok(json!({"state":"unavailable"}))
    }
    async fn provider_picker(&self, _: Context) -> Result<Value, GatewayError> {
        Ok(json!({"providers":[{"id":"codex"}],"tiers":[]}))
    }
    async fn error_diagnostics(&self, _: Context) -> Result<ErrorDiagnostics, GatewayError> {
        Ok(ErrorDiagnostics {
            retention: json!({"state":"unavailable"}),
            log_files: json!({"scope":"host_text_logs","total_bytes":null}),
        })
    }
    async fn pricing(&self, _: &[ModelId]) -> Result<PricingSnapshot, GatewayError> {
        Ok(PricingSnapshot {
            eligible_model_ids: vec!["gpt-source".into()],
            quota_per_credit: 1000,
            model_quota_coefficients: BTreeMap::from([("gpt-source".into(), 1.5)]),
            checked_at_ms: 900000,
        })
    }
}
pub fn input(body: Value) -> AdminInput {
    AdminInput {
        parameter: None,
        query: BTreeMap::new(),
        body: SourceJson::from_serializable(&body)
            .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error:?}")),
        actor: AuthPrincipal {
            token: None,
            method: AuthMethod::AdminAllowlist,
            is_admin: true,
            is_super_admin: false,
        },
        request_id: "admin-fixture".into(),
        now_ms: 1_000_000,
    }
}
pub fn snapshot(context: bool) -> ModelSnapshot {
    ModelSnapshot {
        models: vec![SourceJson::from_serializable(&if context {
            json!({"slug":"gpt-source","context_window":10000,"default_reasoning_level":null,"supported_reasoning_levels":[null,"future-tier"]})
        } else {
            json!({"slug":"gpt-source","default_reasoning_level":null,"supported_reasoning_levels":[null,"future-tier"]})
        }).unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error}"))],
        source: "chatgpt_codex".into(),
        updated_at_ms: 900000,
        client_version: None,
    }
}
pub fn seed_catalog(store: &Store, context: bool) {
    let snapshot = snapshot(context);
    store.rows.borrow_mut().insert(
        codex_snapshot_key().0,
        StoredRecord::json(
            "catalog",
            serde_json::to_value(&snapshot)
                .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error}")),
            None,
        ),
    );
    store.rows.borrow_mut().insert(
        runtime_config_key().0,
        encode_source_control_record(
            "runtime",
            &build_runtime_config(
                &snapshot,
                Some(&ModelId::from_scalar("gpt-source")),
                Some(&JsString16::from_utf8("medium")),
                900000,
            )
            .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error:?}")),
            None,
        )
        .unwrap_or_else(|error| panic!("Invalid admin runtime fixture: {error}")),
    );
}
pub fn runtime<'a>(
    store: &'a Store,
    host: &'a Host,
    config: &'a RuntimeConfig,
) -> AdminRuntime<'a, Store, Host, Capabilities> {
    AdminRuntime {
        store,
        host,
        capabilities: &Capabilities,
        config,
    }
}
