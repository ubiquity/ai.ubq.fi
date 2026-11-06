//! Bounded pages of the authority's current canonical records, in tuple order.
//! The body is the exact JSON array whose SHA-256 the caller must verify.
use crate::{manifest_key, progress_key, refused, root, validate_record};
use gateway_core::error::GatewayError;
use gateway_core::query::{QueryRequest, QuerySelector, QueryStore};
use gateway_migration::staged::CanonicalRecord;
use serde::Deserialize;

pub const EXPORT_PAGE_LIMIT: u32 = 256;
pub const EXPORT_PAGE_MAX_BYTES: usize = 8 * 1024 * 1024;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ExportRequest {
    pub v: u32,
    pub cursor: Option<String>,
    pub limit: u32,
}

#[derive(Debug)]
pub struct ExportPage {
    pub body: Vec<u8>,
    pub count: usize,
    pub next_cursor: Option<String>,
    pub sha256: String,
}

fn encode(records: &[CanonicalRecord]) -> Result<Vec<u8>, GatewayError> {
    serde_json::to_vec(records).map_err(|_| refused("Export page cannot be encoded"))
}

/// Only a sealed, fully staged target exports: a partial target has no
/// accepted inverse. Absolute expiry is kept; `now_ms: 0` filters nothing.
pub async fn export_page<S: QueryStore>(
    store: &S,
    request: ExportRequest,
) -> Result<ExportPage, GatewayError> {
    if request.v != 1 || !(1..=EXPORT_PAGE_LIMIT).contains(&request.limit) {
        return Err(refused("Export page request is invalid"));
    }
    if store.read(&progress_key()).await?.is_some() {
        return Err(refused("A partially staged target cannot be exported"));
    }
    if store.read(&manifest_key()).await?.is_none() {
        return Err(refused("Target has no sealed capture manifest"));
    }
    let query = QueryRequest {
        selector: QuerySelector::Prefix {
            prefix: root()?,
            start: None,
            end: None,
        },
        reverse: false,
        limit: request.limit,
        cursor: request.cursor,
        now_ms: 0,
    };
    let validated = query.validate()?;
    let page = store.query(query).await?;
    let mut records = Vec::with_capacity(page.entries.len());
    for entry in page.entries {
        validate_record(&entry.record)?;
        records.push(CanonicalRecord {
            key: entry.key,
            record: entry.record,
        });
    }
    let mut next_cursor = page.next_cursor;
    let mut body = encode(&records)?;
    while body.len() > EXPORT_PAGE_MAX_BYTES && records.len() > 1 {
        records.pop();
        let last = records
            .last()
            .ok_or_else(|| refused("Export page lost its last record"))?;
        next_cursor = Some(validated.cursor_after(&last.key)?);
        body = encode(&records)?;
    }
    Ok(ExportPage {
        sha256: gateway_auth::crypto::sha256_hex(&body),
        count: records.len(),
        body,
        next_cursor,
    })
}
