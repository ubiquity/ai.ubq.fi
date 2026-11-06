//! Exact-version cache values, real portable gzip and served catalog projections.
use crate::{
    metadata::{MetadataHint, resolved_auto_compact},
    model_control as c,
    util::sha256,
};
use flate2::{Compression, read::GzDecoder, write::GzEncoder};
use gateway_core::{
    error::GatewayError, model::ModelId, source_json::SourceJson, source_serialization as source,
};
use serde::{Deserialize, Serialize};
use std::io::{Read, Write};
pub const FRESH_MS: i64 = 300_000;
pub const RETENTION_MS: i64 = 86_400_000;
pub const REFRESH_LEASE_MS: i64 = 15_000;
pub const COLD_WAIT_MS: i64 = 5_000;
pub const CHUNK_BYTES: usize = 55_000;
pub const MAX_VERSIONS: usize = 32;
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct CatalogMetadata {
    pub client_version: String,
    pub auth_generation: String,
    pub body_generation: String,
    pub content_type: String,
    pub etag: Option<String>,
    pub fetched_at_ms: i64,
    pub chunk_count: usize,
    pub compressed_bytes: usize,
    pub body_bytes: usize,
    pub sha256: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_chunk_count: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_compressed_bytes: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_body_bytes: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub source_sha256: Option<String>,
}
impl CatalogMetadata {
    pub fn valid(&self) -> bool {
        self.chunk_count > 0
            && [
                self.source_chunk_count.is_some(),
                self.source_compressed_bytes.is_some(),
                self.source_body_bytes.is_some(),
                self.source_sha256.is_some(),
            ]
            .iter()
            .all(|v| !*v)
            || self.chunk_count > 0
                && self.source_chunk_count.is_some_and(|n| n > 0)
                && self.source_compressed_bytes.is_some()
                && self.source_body_bytes.is_some()
                && self.source_sha256.is_some()
    }
    pub fn retained(&self, generation: &str, now_ms: i64) -> bool {
        self.valid()
            && self.auth_generation == generation
            && now_ms - self.fetched_at_ms < RETENTION_MS
    }
    pub fn fresh(&self, now_ms: i64) -> bool {
        now_ms - self.fetched_at_ms < FRESH_MS
    }
}
#[derive(Clone, Debug, PartialEq)]
pub struct LoadedCatalog {
    pub metadata: CatalogMetadata,
    pub body: String,
    pub parsed: SourceJson,
    pub source_body: Option<String>,
}
#[derive(Clone, Debug, PartialEq)]
pub struct PreparedCatalog {
    pub metadata: CatalogMetadata,
    pub chunks: Vec<Vec<u8>>,
    pub source_chunks: Vec<Vec<u8>>,
    pub expires_at_ms: i64,
}
pub fn parse_catalog(body: &str) -> Option<SourceJson> {
    let parsed = SourceJson::parse(body).ok()?;
    let models = c::get(&parsed, "models").as_ref().and_then(c::array)?;
    if models.is_empty()
        || !models.iter().all(|model| {
            c::is_record(model)
                && c::identifier(model).is_some_and(|id| !c::trim(&id).as_units().is_empty())
        })
    {
        return None;
    }
    Some(parsed)
}
pub(crate) fn source_stringify(value: &SourceJson) -> String {
    source::canonical(value).map_or_else(|_| value.as_str().to_owned(), |v| v.as_str().to_owned())
}
pub fn gzip(body: &str) -> Result<Vec<u8>, GatewayError> {
    let mut encoder = GzEncoder::new(Vec::new(), Compression::default());
    encoder
        .write_all(body.as_bytes())
        .and_then(|()| encoder.finish())
        .map_err(|_| {
            GatewayError::new(
                502,
                "Codex model catalog could not be compressed",
                Some("codex_catalog_unavailable"),
            )
        })
}
pub fn decode_chunks(
    chunks: &[Vec<u8>],
    compressed_bytes: usize,
    body_bytes: usize,
    digest: &str,
) -> Option<String> {
    if chunks.iter().map(Vec::len).sum::<usize>() != compressed_bytes {
        return None;
    }
    let compressed: Vec<_> = chunks.iter().flatten().copied().collect();
    let decoder = GzDecoder::new(compressed.as_slice());
    let mut bytes = Vec::new();
    decoder
        .take(body_bytes.saturating_add(1) as u64)
        .read_to_end(&mut bytes)
        .ok()?;
    if bytes.len() != body_bytes || sha256(&bytes) != digest {
        return None;
    }
    String::from_utf8(bytes).ok()
}
pub fn widen_catalog_windows(
    body: &str,
    enrichment: &std::collections::BTreeMap<ModelId, MetadataHint>,
) -> Option<String> {
    let mut parsed = parse_catalog(body)?;
    let mut changed = false;
    let mut models = c::get(&parsed, "models").as_ref().and_then(c::array)?;
    for model in &mut models {
        let Some(id) = c::model(model) else {
            continue;
        };
        let Some(hint) = enrichment.get(&id) else {
            continue;
        };
        let endpoint = c::positive(c::get(model, "context_window").as_ref());
        let ceiling = c::positive(c::get(model, "max_context_window").as_ref());
        let widest = [
            endpoint,
            ceiling,
            hint.context_window_tokens,
            hint.max_context_window_tokens,
        ]
        .into_iter()
        .flatten()
        .filter(|n| *n > 0)
        .max();
        let Some(widest) = widest else {
            continue;
        };
        if endpoint.is_none_or(|n| widest > n) {
            c::set(model, "context_window", c::json(&widest));
            changed = true;
        }
        if ceiling.is_none_or(|n| widest > n) {
            c::set(model, "max_context_window", c::json(&widest));
            changed = true;
        }
    }
    if changed {
        c::set(&mut parsed, "models", c::json(&models));
    }
    changed |= apply_compaction_limits(&mut parsed);
    changed.then(|| source_stringify(&parsed))
}
/// Give every positive window the earlier of its source or policy compaction limit.
pub fn apply_compaction_limits(parsed: &mut SourceJson) -> bool {
    let Some(mut models) = c::get(parsed, "models").as_ref().and_then(c::array) else {
        return false;
    };
    let mut changed = false;
    for model in &mut models {
        let Some(window) = c::positive(c::get(model, "context_window").as_ref()) else {
            continue;
        };
        let current = c::get(model, "auto_compact_token_limit");
        let limit = resolved_auto_compact(window, c::positive(current.as_ref()))
            .min(resolved_auto_compact(window, None));
        if current.as_ref().and_then(source::number) != Some(limit as f64) {
            c::set(model, "auto_compact_token_limit", c::json(&limit));
            changed = true;
        }
    }
    if changed {
        c::set(parsed, "models", c::json(&models));
    }
    changed
}
pub struct CatalogIdentity<'a> {
    pub client_version: &'a str,
    pub auth_generation: &'a str,
    pub body_generation: &'a str,
}
pub fn prepare_catalog(
    identity: CatalogIdentity<'_>,
    source_body: &str,
    etag: Option<&str>,
    content_type: Option<&str>,
    fetched_at_ms: i64,
    enrichment: &std::collections::BTreeMap<ModelId, MetadataHint>,
) -> Result<PreparedCatalog, GatewayError> {
    if crate::snapshot::parse_client_version(identity.client_version).is_none()
        || parse_catalog(source_body).is_none()
    {
        return Err(GatewayError::new(
            502,
            "Codex upstream did not return a valid model catalog",
            Some("codex_catalog_unavailable"),
        ));
    }
    let body =
        widen_catalog_windows(source_body, enrichment).unwrap_or_else(|| source_body.to_owned());
    let compressed = gzip(&body)?;
    let source_compressed = gzip(source_body)?;
    let chunks = compressed
        .chunks(CHUNK_BYTES)
        .map(<[u8]>::to_vec)
        .collect::<Vec<_>>();
    let source_chunks = source_compressed
        .chunks(CHUNK_BYTES)
        .map(<[u8]>::to_vec)
        .collect::<Vec<_>>();
    let header = |v: Option<&str>| {
        v.map(crate::util::js_trim)
            .filter(|s| !s.is_empty())
            .map(str::to_owned)
    };
    let metadata = CatalogMetadata {
        client_version: identity.client_version.into(),
        auth_generation: identity.auth_generation.into(),
        body_generation: identity.body_generation.into(),
        content_type: header(content_type).unwrap_or_else(|| "application/json".into()),
        etag: header(etag),
        fetched_at_ms,
        chunk_count: chunks.len(),
        compressed_bytes: compressed.len(),
        body_bytes: body.len(),
        sha256: sha256(body.as_bytes()),
        source_chunk_count: Some(source_chunks.len()),
        source_compressed_bytes: Some(source_compressed.len()),
        source_body_bytes: Some(source_body.len()),
        source_sha256: Some(sha256(source_body.as_bytes())),
    };
    Ok(PreparedCatalog {
        metadata,
        chunks,
        source_chunks,
        expires_at_ms: fetched_at_ms.saturating_add(RETENTION_MS),
    })
}
pub fn client_etag(body: &str) -> String {
    format!("\"uos-catalog-{}\"", &sha256(body.as_bytes())[..32])
}
pub fn conditional_matches(request: Option<&str>, etag: &str) -> bool {
    request.is_some_and(|r| {
        r.split(',').any(|token| {
            matches!(token.trim_matches(crate::util::js_whitespace), "*")
                || token.trim_matches(crate::util::js_whitespace) == etag
        })
    })
}
pub fn requested_client_version<'a>(values: &'a [&str]) -> Option<&'a str> {
    match values {
        [] => None,
        [version] => Some(version),
        _ => Some(""),
    }
}
pub fn validate_client_version(raw: &str) -> Result<String, GatewayError> {
    let version = raw.trim_matches(crate::util::js_whitespace);
    if crate::snapshot::parse_client_version(version).is_none() {
        return Err(GatewayError::new(
            400,
            "client_version must be an exact X.Y.Z version",
            Some("invalid_client_version"),
        )
        .with_param(Some("client_version")));
    }
    Ok(version.into())
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CacheState {
    Hit,
    Stale,
    Wait,
    Rotated,
    Revalidated,
    Miss,
}
impl CacheState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Hit => "hit",
            Self::Stale => "stale",
            Self::Wait => "wait",
            Self::Rotated => "rotated",
            Self::Revalidated => "revalidated",
            Self::Miss => "miss",
        }
    }
}
pub fn prune_versions(catalogs: &[CatalogMetadata], current_version: &str) -> Vec<CatalogMetadata> {
    if catalogs.len() <= MAX_VERSIONS {
        return Vec::new();
    }
    let mut candidates = catalogs.to_vec();
    candidates.sort_by_key(|m| m.fetched_at_ms);
    let mut remaining = candidates.len();
    candidates
        .into_iter()
        .filter(|m| {
            if remaining > MAX_VERSIONS && m.client_version != current_version {
                remaining -= 1;
                true
            } else {
                false
            }
        })
        .collect()
}
#[derive(Clone, Debug, PartialEq)]
pub struct CatalogResponse {
    pub status: u16,
    pub body: Option<String>,
    pub headers: std::collections::BTreeMap<String, String>,
}
pub fn catalog_response(
    catalog: &LoadedCatalog,
    conditional: Option<&str>,
    cache_state: CacheState,
    input: &crate::catalog::CatalogInput,
) -> CatalogResponse {
    let active_whitelist = input
        .whitelist
        .as_ref()
        .is_some_and(|w| !w.model_ids.is_empty());
    let paid = input
        .available_rows("openlux")
        .iter()
        .chain(input.available_rows("surplus"))
        .any(|r| crate::catalog::endpoint_supports(r, "openai-response"));
    let stored_only = !active_whitelist
        && !paid
        && input.available_rows("claude_code").is_empty()
        && !["deepseek", "lithos", "cerebras"]
            .iter()
            .any(|p| input.enabled(p))
        && !input.synthetic_enabled()
        && !(input.enabled("openrouter") && input.configured("openrouter"))
        && input.enabled("codex");
    let body = if stored_only {
        catalog.body.clone()
    } else {
        source_stringify(&crate::native_catalog::native_catalog(
            Some(&catalog.parsed),
            input,
        ))
    };
    let etag = client_etag(&body);
    let not_modified = conditional_matches(conditional, &etag);
    let headers = [
        ("Content-Type".into(), catalog.metadata.content_type.clone()),
        (
            "Cache-Control".into(),
            "private, max-age=300, stale-while-revalidate=86400".into(),
        ),
        ("x-uos-upstream".into(), "chatgpt_codex".into()),
        ("x-uos-cache".into(), cache_state.as_str().into()),
        ("ETag".into(), etag),
    ]
    .into_iter()
    .collect();
    CatalogResponse {
        status: if not_modified { 304 } else { 200 },
        body: (!not_modified).then_some(body),
        headers,
    }
}

pub fn fallback_response(input: &crate::catalog::CatalogInput) -> Option<CatalogResponse> {
    let body = crate::native_catalog::native_catalog_fallback(input)?;
    Some(CatalogResponse {
        status: 200,
        body: Some(source_stringify(&body)),
        headers: [
            ("Content-Type".into(), "application/json".into()),
            ("Cache-Control".into(), "private, max-age=300".into()),
            ("x-uos-upstream".into(), "metered".into()),
        ]
        .into_iter()
        .collect(),
    })
}
/// A rotation during fenced publication cannot reuse an old generation or invent a fallback.
pub fn rotation_unavailable() -> GatewayError {
    GatewayError::new(
        502,
        "Codex authentication changed during catalog refresh",
        Some("codex_catalog_unavailable"),
    )
}
