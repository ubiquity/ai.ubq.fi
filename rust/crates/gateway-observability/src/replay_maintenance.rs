//! Original replay maintenance orchestration; each effect remains with its finite owner.
use crate::ObservabilityRuntime;
use gateway_core::query::QueryStore;
use gateway_observability_contracts::{data, types::ObservabilityHost};
use gateway_observability_replay_model::replay_model as model;
use gateway_observability_replay_records::{accounting::self as schema, accounting::RetentionLimits};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
impl<S: QueryStore,H: ObservabilityHost> ObservabilityRuntime<S,H> {
    pub async fn replay_retention_status(&self, limits: RetentionLimits) -> Value {
        match schema::read_ledger(self.store.as_ref(), limits).await {
            Ok((ledger, _)) => {
                let used = ledger.stored_bytes + ledger.reserved_bytes;
                json!({"state":"ok","scope":"capture_owned_kv_payload","budget_bytes":limits.budget(),"stored_bytes":ledger.stored_bytes,"reserved_bytes":ledger.reserved_bytes,"records":ledger.records,"metadata_bytes":ledger.metadata_bytes,"status_records":ledger.status_records,"evicted_records":ledger.evicted_records,"evicted_bytes":ledger.evicted_bytes,"expired_records":ledger.expired_records,"last_eviction_at_ms":ledger.last_eviction_at_ms,"last_warning_at_ms":ledger.last_warning_at_ms,"over_budget":used>limits.payload(),"near_capacity":used>=(limits.payload()*0.9).floor(),"accounting_complete":ledger.ready(),"accounting_error":ledger.accounting_error,"skipped_reason":ledger.last_skip_reason})
            }
            Err(error) => {
                json!({"state":if error.envelope.error.code.as_deref()==Some("observability_state_invalid"){"ok"}else{"unavailable"},"scope":"capture_owned_kv_payload","budget_bytes":limits.budget(),"stored_bytes":null,"reserved_bytes":null,"records":null,"metadata_bytes":null,"status_records":null,"evicted_records":null,"evicted_bytes":null,"expired_records":null,"last_eviction_at_ms":null,"last_warning_at_ms":null,"over_budget":false,"near_capacity":false,"accounting_complete":false,"accounting_error":"ledger_corrupt","skipped_reason":"storage_accounting_in_progress"})
            }
        }
    }
    pub async fn maintain_replays(&self, limits: RetentionLimits) -> Result<Value, GatewayError> {
        let at = data::now(self.host.as_ref())?;
        if !self.replay_state().bootstrap_replays(limits, at).await? {
            return Ok(self.replay_retention_status(limits).await);
        }
        self.retention().reap_replay_reservations(at, limits).await?;
        self.retention().reclaim_expired_replays(at, limits).await?;
        let (ledger, _) = schema::read_ledger(self.store.as_ref(), limits).await?;
        if ledger.stored_bytes + ledger.reserved_bytes > limits.payload() {
            self.retention().evict_replays(at, limits, (limits.payload() * 0.9).floor(), None)
                .await?;
        }
        let marker = self.store.read(&model::key("metadata_reconciled")).await?;
        let due = data::value(marker.as_ref(), at)?
            .and_then(|v| v["reconciled_at_ms"].as_f64())
            .is_none_or(|t| at as f64 - t >= 900_000.0);
        if due {
            self.replay_state()
                .recount_replay_metadata(limits, at)
                .await?;
        }
        Ok(self.replay_retention_status(limits).await)
    }
}
