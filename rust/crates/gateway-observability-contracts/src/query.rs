//! Closed observation reads. Generic cursor/query machinery remains private.
use crate::data;
use gateway_core::{
    query::QueryEntry, query::QueryRequest, query::QuerySelector, query::QueryStore,
};
use gateway_source::error::GatewayError;
use serde_json::{Value, json};
#[derive(Clone, Copy)]
pub enum PromptNamespace {
    All,
    Dimension,
    Overflow,
    Meta,
    Legacy,
}
impl PromptNamespace {
    pub fn name(self) -> &'static str {
        match self {
            Self::All => "all",
            Self::Dimension => "dimension",
            Self::Overflow => "overflow",
            Self::Meta => "meta",
            Self::Legacy => "legacy",
        }
    }
}
#[derive(Clone, Copy)]
pub enum CapacityRows {
    History,
    ResetEvent,
    RateLimitResetEvent,
    DowntimeEvent,
}
impl CapacityRows {
    fn name(self) -> &'static str {
        match self {
            Self::History => "history",
            Self::ResetEvent => "reset_event",
            Self::RateLimitResetEvent => "rate_limit_reset_event",
            Self::DowntimeEvent => "downtime_event",
        }
    }
}
#[derive(Clone, Copy)]
pub enum ReplayRows {
    Accounting,
    Request,
    Evicted,
}
impl ReplayRows {
    fn name(self) -> &'static str {
        match self {
            Self::Accounting => "accounting",
            Self::Request => "request",
            Self::Evicted => "evicted",
        }
    }
}
pub enum ObservationRead<'a> {
    RequestLedger(&'a str),
    LedgerDays {
        start: i64,
        end: i64,
    },
    UsageRollups {
        start: i64,
        end: i64,
    },
    BalanceHistory {
        fingerprint: &'a str,
        start: i64,
        end: i64,
    },
    Prompt {
        namespace: PromptNamespace,
        before: Option<i64>,
    },
    Capacity(CapacityRows),
    CapacityRollups {
        start: i64,
        end: i64,
    },
    RemoteCapacity,
    ResetShadow,
    ResetRedemption,
    Replay(ReplayRows),
    ReplayChunks(&'a str),
}
fn range(mut parts: Vec<Value>, start: i64, end: i64) -> QuerySelector {
    let mut first = parts.clone();
    first.push(json!(start));
    parts.push(json!(end));
    QuerySelector::Range {
        start: data::key(json!(first)),
        end: data::key(json!(parts)),
    }
}
fn prefix(parts: Value) -> QuerySelector {
    QuerySelector::Prefix {
        prefix: data::key(parts),
        start: None,
        end: None,
    }
}
fn selector(read: ObservationRead<'_>) -> QuerySelector {
    match read {
        ObservationRead::RequestLedger(key) => {
            prefix(json!(["uos_ai", "paid_fallback", "ledger", key]))
        }
        ObservationRead::LedgerDays { start, end } => range(
            vec![
                json!("uos_ai"),
                json!("paid_fallback"),
                json!("v3"),
                json!("ledger_stats"),
            ],
            start,
            end,
        ),
        ObservationRead::UsageRollups { start, end } => range(
            vec![
                json!("uos_ai"),
                json!("paid_fallback"),
                json!("v3"),
                json!("usage_rollup"),
            ],
            start,
            end,
        ),
        ObservationRead::BalanceHistory {
            fingerprint,
            start,
            end,
        } => range(
            vec![
                json!("uos_ai"),
                json!("metered_quota"),
                json!("v1"),
                json!("balance_history"),
                json!(fingerprint),
            ],
            start,
            end,
        ),
        ObservationRead::Prompt { namespace, before } => {
            let mut parts = vec![json!("uos_ai"), json!("prompt_cache_analytics")];
            if matches!(namespace, PromptNamespace::Legacy) {
                parts.push(json!("v1"));
            } else {
                parts.extend([json!("v2"), json!(namespace.name())]);
            }
            let key = data::key(json!(parts));
            let end = before.map(|at| {
                parts.push(json!(at));
                data::key(json!(parts))
            });
            QuerySelector::Prefix {
                prefix: key,
                start: None,
                end,
            }
        }
        ObservationRead::Capacity(kind) => {
            prefix(json!(["uos_ai", "provider_capacity", "v1", kind.name()]))
        }
        ObservationRead::CapacityRollups { start, end } => range(
            vec![
                json!("uos_ai"),
                json!("provider_capacity"),
                json!("v1"),
                json!("rollup"),
            ],
            start,
            end,
        ),
        ObservationRead::RemoteCapacity => prefix(json!([
            "uos_ai",
            "provider_capacity",
            "analytics",
            "remote"
        ])),
        ObservationRead::ResetShadow => {
            prefix(json!(["uos_ai", "codex_reset_shadow_decision", "v1"]))
        }
        ObservationRead::ResetRedemption => {
            prefix(json!(["uos_ai", "codex_reset_redemption", "v1"]))
        }
        ObservationRead::Replay(kind) => {
            prefix(json!(["uos_ai", "sentinel_replay", "v1", kind.name()]))
        }
        ObservationRead::ReplayChunks(id) => {
            prefix(json!(["uos_ai", "sentinel_replay", "v1", "chunk", id]))
        }
    }
}
pub async fn scan<S: QueryStore>(
    store: &S,
    read: ObservationRead<'_>,
    reverse: bool,
    at: i64,
    limit: usize,
) -> Result<Vec<QueryEntry>, GatewayError> {
    scan_selector(store, selector(read), reverse, at, limit).await
}
async fn scan_selector<S: QueryStore>(
    store: &S,
    selector: QuerySelector,
    reverse: bool,
    at: i64,
    limit: usize,
) -> Result<Vec<QueryEntry>, GatewayError> {
    let mut cursor = None;
    let mut rows = Vec::new();
    loop {
        let request = QueryRequest {
            selector: selector.clone(),
            reverse,
            limit: (limit.saturating_sub(rows.len())).clamp(1, 256) as u32,
            cursor: cursor.clone(),
            now_ms: at,
        };
        let page = store.query(request).await?;
        if page.next_cursor.is_some() && page.next_cursor == cursor {
            return Err(data::error("Observation scan cursor did not advance"));
        }
        rows.extend(page.entries);
        if rows.len() >= limit {
            rows.truncate(limit);
            break;
        }
        let Some(next) = page.next_cursor else { break };
        cursor = Some(next);
    }
    Ok(rows)
}
