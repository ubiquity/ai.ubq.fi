use crate::HistoryHost;
use gateway_source::error::GatewayError;
use gateway_core::query::{QueryEntry, QueryRequest, QuerySelector, QueryStore};
pub(crate) fn error(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "observability_state_invalid")
}
pub(crate) fn now<H: HistoryHost>(host: &H) -> Result<i64, GatewayError> {
    let at = host.now_ms();
    if (0..=gateway_contracts::identity::MAX_SAFE_INTEGER).contains(&at) {
        Ok(at)
    } else {
        Err(error("Invalid observation clock"))
    }
}
pub(crate) fn uuid<H: HistoryHost>(host: &H) -> Result<String, GatewayError> {
    gateway_auth::crypto::uuid_from_random(&host.random_bytes(16)?)
}
pub(crate) async fn scan_selector<S: QueryStore>(
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
            return Err(error("Observation scan cursor did not advance"));
        }
        rows.extend(page.entries);
        if rows.len() >= limit {
            rows.truncate(limit);
            break;
        }
        let Some(next) = page.next_cursor else {
            break;
        };
        cursor = Some(next);
    }
    Ok(rows)
}
