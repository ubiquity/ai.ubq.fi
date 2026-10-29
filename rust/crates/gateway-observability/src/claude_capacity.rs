//! Composition of accepted observations with the extracted retained-history owner.
use crate::ObservabilityRuntime;
use gateway_claude_usage_history::{History, HistoryHost};
use gateway_claude_usage_history_records::types::ClaudeAcceptedObservation;
use gateway_core::query::QueryStore;
use gateway_observability_contracts::types::ObservabilityHost;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
struct Host<'a, H>(&'a H);
impl<H: ObservabilityHost> HistoryHost for Host<'_, H> {
    fn now_ms(&self) -> i64 {
        self.0.now_ms()
    }
    fn random_bytes(&self, n: usize) -> Result<Vec<u8>, GatewayError> {
        self.0.random_bytes(n)
    }
    fn parse_date(&self, s: &JsString16) -> Option<i64> {
        self.0.parse_claude_usage_date(s)
    }
}
impl<S: QueryStore, H: ObservabilityHost> ObservabilityRuntime<S, H> {
    pub async fn record_claude_capacity_observation(
        &self,
        event: &ClaudeAcceptedObservation,
    ) -> Result<bool, GatewayError> {
        History {
            store: self.store.as_ref(),
            host: &Host(self.host.as_ref()),
        }
        .record_claude_capacity_observation(event)
        .await
    }
    pub async fn claude_capacity_history(&self, at: i64) -> SourceJson {
        History {
            store: self.store.as_ref(),
            host: &Host(self.host.as_ref()),
        }
        .claude_capacity_history(at)
        .await
    }
    pub async fn claude_capacity_rollups(&self, since: i64, at: i64) -> SourceJson {
        History {
            store: self.store.as_ref(),
            host: &Host(self.host.as_ref()),
        }
        .claude_capacity_rollups(since, at)
        .await
    }
    pub async fn claude_usage_rollups(&self, since: i64, at: i64) -> SourceJson {
        History {
            store: self.store.as_ref(),
            host: &Host(self.host.as_ref()),
        }
        .usage_rollups(since, at)
        .await
    }
}
