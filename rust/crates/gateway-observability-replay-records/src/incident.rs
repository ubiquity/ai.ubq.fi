//! Exact named incident source DATA and predicates; never live capture or authority proof.
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_core::state::RecordKey;
use gateway_observability_contracts::{data, source_object::SourceObject};
use gateway_observability_replay_model::replay_model as model;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};
use serde_json::json;
pub fn valid_incident_id(value: &str) -> bool {
    let Some(uuid) = value.strip_prefix("provider-") else {
        return false;
    };
    let bytes = uuid.as_bytes();
    bytes.len() == 36
        && bytes.iter().enumerate().all(|(i, b)| {
            if [8, 13, 18, 23].contains(&i) {
                *b == b'-'
            } else {
                b.is_ascii_digit() || (b'a'..=b'f').contains(b)
            }
        })
        && bytes[14] == b'4'
        && b"89ab".contains(&bytes[19])
}
pub const MESSAGE: &str = "Gateway observed a provider failure for this incident group.";
const FIELDS: &[&str] = &[
    "version",
    "incident_id",
    "fingerprint",
    "severity",
    "first_seen_at_ms",
    "last_seen_at_ms",
    "count",
    "failing_revision",
    "error_type",
    "context",
    "provenance",
    "evidence_ref",
    "evidence_expires_at_ms",
    "latest",
];
pub fn full_sha(s: &str) -> bool {
    s.len() == 40
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
pub fn positive(n: f64) -> bool {
    model::counter(n) && n > 0.0
}
pub fn slug(s: &str, max: usize) -> bool {
    !s.is_empty()
        && s.len() <= max
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.:-".contains(&b))
}
pub fn endpoint(s: &str) -> &str {
    match s.split('?').next().unwrap_or("") {
        "/v1/responses" => "/v1/responses",
        "/v1/chat/completions" => "/v1/chat/completions",
        _ => "other",
    }
}
#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Terminal {
    StreamFraming,
    HttpError,
    ResponseFailed,
    ResponseIncomplete,
    Error,
    Deadline,
    Eof,
    Unknown,
}
#[derive(Clone, Debug)]
pub struct IncidentObservation {
    pub endpoint: String,
    pub method: String,
    pub git_sha: String,
    pub observed_at_ms: i64,
    pub status: u16,
    pub stream: bool,
    pub completed: bool,
    pub framing_valid: bool,
    pub terminal_type: Option<String>,
    pub provider: Option<String>,
    pub model: Option<JsString16>,
    pub reasoning: Option<JsString16>,
    pub failure_kind: Option<String>,
}
impl IncidentObservation {
    pub fn terminal(&self) -> Terminal {
        if self.stream && !self.framing_valid {
            return Terminal::StreamFraming;
        }
        match self.terminal_type.as_deref() {
            Some("http.error") => Terminal::HttpError,
            Some("response.failed") => Terminal::ResponseFailed,
            Some("response.incomplete") => Terminal::ResponseIncomplete,
            Some("error") => Terminal::Error,
            Some("deadline") => Terminal::Deadline,
            Some("eof") => Terminal::Eof,
            _ => Terminal::Unknown,
        }
    }
    pub fn fingerprint(&self) -> Result<String, GatewayError> {
        #[derive(Serialize)]
        struct Classification<'a> {
            endpoint: &'a str,
            method: &'a str,
            status: u16,
            stream: bool,
            completed: bool,
            framing_valid: bool,
            terminal: Terminal,
        }
        if self.status > 599 {
            return Err(data::error(
                "Sentinel incident classification status is invalid",
            ));
        }
        let value = Classification {
            endpoint: endpoint(&self.endpoint),
            method: if self.method == "POST" {
                "POST"
            } else {
                "other"
            },
            status: self.status,
            stream: self.stream,
            completed: self.completed,
            framing_valid: self.framing_valid,
            terminal: self.terminal(),
        };
        Ok(sha256_hex(
            SourceJson::from_serializable(&value)?.as_bytes(),
        ))
    }
    pub fn latest(&self) -> Latest {
        let bounded = |v: &Option<String>, max| v.as_ref().filter(|s| slug(s, max)).cloned();
        let source_slug = |v: &Option<JsString16>, max| {
            v.as_ref()
                .and_then(|s| s.try_utf8().ok())
                .filter(|s| slug(s, max))
        };
        Latest {
            observed_at_ms: self.observed_at_ms as f64,
            revision: full_sha(&self.git_sha).then(|| self.git_sha.clone()),
            provider: bounded(&self.provider, 128),
            route: endpoint(&self.endpoint).into(),
            model: source_slug(&self.model, 128),
            reasoning: source_slug(&self.reasoning, 32),
            failure_kind: bounded(&self.failure_kind, 128),
            terminal: self.terminal(),
            status: f64::from(self.status),
            stream: self.stream,
        }
    }
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Latest {
    pub observed_at_ms: f64,
    #[serde(deserialize_with = "model::nullable")]
    pub revision: Option<String>,
    #[serde(deserialize_with = "model::nullable")]
    pub provider: Option<String>,
    pub route: String,
    #[serde(deserialize_with = "model::nullable")]
    pub model: Option<String>,
    #[serde(deserialize_with = "model::nullable")]
    pub reasoning: Option<String>,
    #[serde(deserialize_with = "model::nullable")]
    pub failure_kind: Option<String>,
    pub terminal: Terminal,
    pub status: f64,
    pub stream: bool,
}
impl Latest {
    pub fn valid(&self) -> bool {
        positive(self.observed_at_ms)
            && self.revision.as_deref().is_none_or(full_sha)
            && self.provider.as_ref().is_none_or(|s| slug(s, 128))
            && matches!(
                self.route.as_str(),
                "/v1/responses" | "/v1/chat/completions" | "other"
            )
            && self.model.as_ref().is_none_or(|s| slug(s, 128))
            && self.reasoning.as_ref().is_none_or(|s| slug(s, 32))
            && self.failure_kind.as_ref().is_none_or(|s| slug(s, 128))
            && model::counter(self.status)
            && self.status <= 599.0
    }
}
#[derive(Clone, Deserialize, Serialize)]
pub struct ContextMessage {
    pub message: String,
    pub location: (),
    pub sample: Vec<SourceJson>,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct Provenance {
    pub endpoint: String,
    pub captured_at_ms: f64,
    pub captured_by: (),
}
#[derive(Clone, Deserialize, Serialize)]
pub struct EvidenceRef {
    pub r#ref: String,
    #[serde(deserialize_with = "model::nullable")]
    pub digest: Option<String>,
}
#[derive(Clone, Deserialize, Serialize)]
pub struct IncidentIndexData {
    pub version: f64,
    pub incident_id: String,
    pub fingerprint: String,
    pub severity: String,
    pub first_seen_at_ms: f64,
    pub last_seen_at_ms: f64,
    pub count: f64,
    #[serde(deserialize_with = "model::nullable")]
    pub failing_revision: Option<String>,
    pub error_type: Terminal,
    pub context: ContextMessage,
    pub provenance: Provenance,
    #[serde(deserialize_with = "model::nullable")]
    pub evidence_ref: Option<EvidenceRef>,
    #[serde(deserialize_with = "model::nullable")]
    pub evidence_expires_at_ms: Option<f64>,
    #[serde(
        default,
        skip_serializing_if = "Option::is_none",
        deserialize_with = "model::optional_nonnull"
    )]
    pub latest: Option<Latest>,
}
impl IncidentIndexData {
    pub fn parse(source: &SourceJson) -> Result<Self, GatewayError> {
        let row: Self = SourceObject::parse(source)?.select(FIELDS)?.deserialize()?;
        if !row.valid() {
            return Err(data::error("Sentinel incident index record is invalid"));
        }
        Ok(row)
    }
    pub fn valid(&self) -> bool {
        self.version == 1.0
            && valid_incident_id(&self.incident_id)
            && model::digest(&self.fingerprint)
            && self.severity == "P2"
            && positive(self.first_seen_at_ms)
            && positive(self.last_seen_at_ms)
            && self.last_seen_at_ms >= self.first_seen_at_ms
            && positive(self.count)
            && self.failing_revision.as_deref().is_none_or(full_sha)
            && self.context.message == MESSAGE
            && self.context.sample.is_empty()
            && matches!(
                self.provenance.endpoint.as_str(),
                "/v1/responses" | "/v1/chat/completions" | "other"
            )
            && positive(self.provenance.captured_at_ms)
            && self.evidence_ref.as_ref().is_none_or(|v| {
                v.r#ref
                    .strip_prefix("capture:")
                    .is_some_and(model::capture_id)
                    && v.digest.as_deref().is_none_or(model::digest)
            })
            && self.evidence_expires_at_ms.is_none_or(positive)
            && self.evidence_ref.is_some() == self.evidence_expires_at_ms.is_some()
            && self.latest.as_ref().is_none_or(Latest::valid)
    }
}

pub fn capture_index_document(
    source: &SourceJson,
    manifest: &model::Manifest,
    git_sha: &str,
    ciphertext_sha256: &str,
) -> Result<(SourceJson, String), GatewayError> {
    let row = IncidentIndexData::parse(&source)?;
    let mut document = SourceObject::parse(&source)?;
    document.set("failing_revision", &full_sha(git_sha).then_some(git_sha))?;
    let mut provenance = SourceObject::parse(
        document
            .get("provenance")
            .ok_or_else(|| data::error("Incident provenance missing"))?,
    )?;
    provenance.set("captured_at_ms", &manifest.captured_at_ms)?;
    document.set("provenance", &provenance.source_json()?)?;
    document.set(
        "evidence_ref",
        &EvidenceRef {
            r#ref: format!("capture:{}", manifest.capture_id),
            digest: Some(ciphertext_sha256.into()),
        },
    )?;
    document.set("evidence_expires_at_ms", &manifest.expires_at_ms)?;
    let document = document.source_json()?;
    IncidentIndexData::parse(&document)?;
    Ok((document, row.incident_id))
}

pub fn index_key(digest: &str) -> RecordKey {
    data::key(json!([
        "uos_ai",
        "sentinel_incident",
        "v1",
        "index",
        digest
    ]))
}
