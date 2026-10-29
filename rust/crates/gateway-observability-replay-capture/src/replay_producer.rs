//! Source request-owned failure capture. No caller JSON can mint this transport input.
use crate::ReplayCapture;
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_core::{query::QueryStore, state::StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_observability_contracts::{
    data, host_types::BuildIdentity, source_object::SourceObject, types::ObservabilityHost,
};
use gateway_observability_incidents::incidents::IncidentEvidence;
use gateway_observability_replay_model::{
    records::read_json_record, replay_crypto, replay_crypto::FingerprintInput,
    replay_crypto::ReplayBody, replay_crypto::ReplayKey, replay_model as model,
};
use gateway_observability_replay_observation::{
    observation::ClientFailureObservation, observation::FailureObservation, observation::bounded,
    observation::bounded_param, observation::should_persist, observation::should_signal,
    replay_trace::UpstreamTrace,
};
use gateway_observability_replay_records::{
    accounting as schema, accounting::RetentionLimits, incident::IncidentObservation,
};
use gateway_observability_replay_retention::replay_retention::Admission;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
use std::collections::BTreeMap;
use zeroize::Zeroizing;
pub const COMPATIBILITY_HEADERS: &[&str] = &[
    "accept",
    "openai-beta",
    "openai-organization",
    "openai-project",
    "originator",
    "user-agent",
    "x-codex-client-version",
    "x-stainless-arch",
    "x-stainless-lang",
    "x-stainless-os",
    "x-stainless-package-version",
    "x-stainless-retry-count",
    "x-stainless-runtime",
    "x-stainless-runtime-version",
];
pub struct ReplayRequest {
    pub endpoint: String,
    pub method: String,
    pub body: ReplayBody,
    pub content_type: Option<String>,
    pub headers: BTreeMap<String, String>,
    pub request_id: String,
    pub upstream: UpstreamTrace,
}
pub struct ReplayInput {
    endpoint: String,
    method: String,
    body: ReplayBody,
    content_type: Option<String>,
    compatibility_headers: BTreeMap<String, String>,
    request_id: String,
    build: BuildIdentity,
    upstream: UpstreamTrace,
}
impl ReplayInput {
    pub fn new<H: ObservabilityHost>(
        host: &H,
        request: ReplayRequest,
    ) -> Result<Self, GatewayError> {
        if request.method != "POST"
            || request.body.as_bytes().len() > replay_crypto::MAX_REQUEST_BYTES
            || !model::request_id(&request.request_id)
        {
            return Err(data::error(
                "Replay input is not an accepted bounded POST body",
            ));
        }
        let mut headers = BTreeMap::new();
        for name in COMPATIBILITY_HEADERS {
            if let Some(value) = request
                .headers
                .iter()
                .find(|(key, _)| key.eq_ignore_ascii_case(name))
                .map(|(_, v)| v.trim_matches(gateway_schema_values::scalar::js_whitespace))
                .filter(|v| !v.is_empty())
            {
                headers.insert((*name).into(), value.into());
            }
        }
        let build = host.build_identity()?;
        build.validate()?;
        Ok(Self {
            endpoint: request.endpoint,
            method: request.method,
            body: request.body,
            content_type: request
                .content_type
                .map(|v| {
                    v.trim_matches(gateway_schema_values::scalar::js_whitespace)
                        .to_owned()
                })
                .filter(|v| !v.is_empty()),
            compatibility_headers: headers,
            request_id: request.request_id,
            build,
            upstream: request.upstream,
        })
    }
    pub fn request_id(&self) -> &str {
        &self.request_id
    }
}
#[derive(Clone, Debug)]
pub enum PersistResult {
    Stored {
        manifest: model::Manifest,
        manifest_key: RecordKey,
    },
    Duplicate {
        fingerprint: String,
        manifest_key: RecordKey,
    },
    Disabled {
        reason: &'static str,
    },
    Incomplete {
        reason: &'static str,
    },
}
fn settings(input: &ReplayInput, observation: &FailureObservation) -> serde_json::Value {
    let mut out = serde_json::json!({"source":"unavailable","provider_route":observation.provider_route,"model_requested":null,"reasoning_requested":null,"stream_requested":null,"stream_observed":observation.stream});
    let decoded = String::from_utf8_lossy(input.body.as_bytes());
    let parsed = SourceJson::parse(decoded.strip_prefix('\u{feff}').unwrap_or(&decoded))
        .and_then(|v| SourceObject::parse(&v));
    if let Ok(parsed) = parsed {
        out["source"] = serde_json::json!("recorded_request_body");
        out["model_requested"] = serde_json::json!(bounded(
            parsed
                .get("model")
                .and_then(|v| v.deserialize::<String>().ok())
                .as_deref(),
            false
        ));
        let reasoning = parsed
            .get("reasoning")
            .and_then(|v| SourceObject::parse(v).ok())
            .and_then(|v| v.get("effort").and_then(|v| v.deserialize::<String>().ok()));
        let fallback = parsed
            .get("reasoning_effort")
            .and_then(|v| v.deserialize::<String>().ok());
        out["reasoning_requested"] = serde_json::json!(
            bounded(reasoning.as_deref(), false).or_else(|| bounded(fallback.as_deref(), false))
        );
        out["stream_requested"] = serde_json::json!(
            parsed
                .get("stream")
                .and_then(|v| v.deserialize::<bool>().ok())
        );
    }
    out
}
#[derive(Serialize)]
struct Downstream<'a> {
    terminal_type: &'a Option<String>,
    failure_kind: &'a Option<String>,
    error_code: &'a Option<String>,
    error_param: &'a Option<JsString16>,
    body_base64: &'a Option<String>,
    body_truncated: bool,
}
#[derive(Serialize)]
struct Metadata<'a> {
    version: u8,
    captured_at_ms: i64,
    endpoint: &'a str,
    method: &'a str,
    content_type: &'a Option<String>,
    compatibility_headers: &'a BTreeMap<String, String>,
    failure_signature: &'a str,
    observation: &'a FailureObservation,
    client_observation: &'a ClientFailureObservation,
    request_id: &'a str,
    git_sha: &'a str,
    deno_revision: &'a str,
    upstream: &'a UpstreamTrace,
    settings: serde_json::Value,
    capture_status: &'static str,
    replay_coverage: &'static str,
    unavailable: Vec<&'static str>,
    body_sha256: String,
    body_bytes: usize,
    downstream: Downstream<'a>,
}
struct DuplicateCapture<'a> {
    fingerprint: &'a str,
    dedupe: &'a gateway_core::state::StoredRecord,
    index: Option<&'a str>,
    at: i64,
    limits: RetentionLimits,
    state: model::CaptureState,
}
struct CaptureEvent<'a> {
    input: &'a ReplayInput,
    observation: &'a FailureObservation,
    client: &'a ClientFailureObservation,
    key: &'a ReplayKey,
    index: Option<&'a str>,
    at: i64,
    limits: RetentionLimits,
}
impl<S: QueryStore, H: ObservabilityHost> ReplayCapture<'_, S, H> {
    pub async fn record_capture_omission(
        &self,
        request_id: &str,
        reason: &str,
    ) -> Result<bool, GatewayError> {
        if !model::request_id(request_id)
            || !matches!(
                reason,
                "body_over_limit" | "body_unavailable" | "non_post" | "rejected_before_capture"
            )
        {
            return Err(data::error("Replay capture omission is invalid"));
        }
        self.replay_state()
            .admit_replay_status(
                &model::Status {
                    version: 1.0,
                    request_id: request_id.into(),
                    status: model::CaptureState::Disabled,
                    reason: Some(reason.into()),
                    captured_at_ms: data::now(self.host)? as f64,
                    manifest_key: None,
                    fingerprint: None,
                    expires_at_ms: None,
                },
                RetentionLimits::default(),
            )
            .await
    }
    pub async fn persist_failure(
        &self,
        input: ReplayInput,
        observation: FailureObservation,
        client: ClientFailureObservation,
        key: Option<&ReplayKey>,
    ) -> Result<PersistResult, GatewayError> {
        self.persist_failure_with_limits(
            input,
            observation,
            client,
            key,
            RetentionLimits::default(),
        )
        .await
    }
    pub async fn persist_failure_with_limits(
        &self,
        input: ReplayInput,
        observation: FailureObservation,
        mut client: ClientFailureObservation,
        key: Option<&ReplayKey>,
        limits: RetentionLimits,
    ) -> Result<PersistResult, GatewayError> {
        if !should_persist(&observation, &client) {
            return Err(data::error(
                "A successful or cancelled request cannot be persisted as a replay",
            ));
        }
        let build = self.host.build_identity()?;
        if build != input.build {
            return Err(data::error(
                "Replay capture belongs to a different immutable build",
            ));
        }
        client.error_code = bounded(client.error_code.as_deref(), false);
        client.error_param = bounded_param(client.error_param.as_ref());
        let at = data::now(self.host)?;
        let index = if should_signal(&observation, &client) {
            self.incidents()
                .record_incident_index(&IncidentObservation {
                    endpoint: input.endpoint.clone(),
                    method: input.method.clone(),
                    git_sha: build.git_sha.clone(),
                    observed_at_ms: at,
                    status: client.status,
                    stream: client.stream,
                    completed: client.completed,
                    framing_valid: client.framing_valid,
                    terminal_type: client.terminal_type.clone(),
                    provider: Some(client.provider_route.clone()),
                    model: None,
                    reasoning: None,
                    failure_kind: client.failure_kind.clone(),
                })
                .await
                .ok()
        } else {
            None
        };
        let Some(key) = key else {
            let _ = self
                .capture_status_outcome(
                    &input.request_id,
                    model::CaptureState::Disabled,
                    "key_missing",
                    at,
                    limits,
                )
                .await;
            return Ok(PersistResult::Disabled {
                reason: "key_missing",
            });
        };
        let result = self
            .persist_encrypted_failure(CaptureEvent {
                input: &input,
                observation: &observation,
                client: &client,
                key,
                index: index.as_ref().map(|i| i.fingerprint.as_str()),
                at,
                limits,
            })
            .await;
        match &result {
            Ok(PersistResult::Incomplete { reason }) => {
                let _ = self
                    .capture_status_outcome(
                        &input.request_id,
                        model::CaptureState::Disabled,
                        reason,
                        at,
                        limits,
                    )
                    .await;
            }
            Err(_) => {
                let _ = self
                    .capture_status_outcome(
                        &input.request_id,
                        model::CaptureState::Failed,
                        "persist_failed",
                        at,
                        limits,
                    )
                    .await;
            }
            _ => {}
        }
        result
    }
    async fn capture_status_outcome(
        &self,
        id: &str,
        state: model::CaptureState,
        reason: &str,
        at: i64,
        limits: RetentionLimits,
    ) -> Result<bool, GatewayError> {
        self.replay_state()
            .admit_replay_status(
                &model::Status {
                    version: 1.0,
                    request_id: id.into(),
                    status: state,
                    reason: Some(reason.into()),
                    captured_at_ms: at as f64,
                    manifest_key: None,
                    fingerprint: None,
                    expires_at_ms: None,
                },
                limits,
            )
            .await
    }
    async fn persist_encrypted_failure(
        &self,
        event: CaptureEvent<'_>,
    ) -> Result<PersistResult, GatewayError> {
        let CaptureEvent {
            input,
            observation,
            client,
            key,
            index,
            at,
            limits,
        } = event;
        let signature = client.signature()?;
        let canonical = input.upstream.canonical()?;
        let mut headers = input.compatibility_headers.iter().collect::<Vec<_>>();
        let mut error = None;
        headers.sort_by(|a, b| match self.host.compare_labels(a.0, b.0) {
            Ok(order) => order,
            Err(e) => {
                error = Some(e);
                std::cmp::Ordering::Equal
            }
        });
        if let Some(error) = error {
            return Err(error);
        }
        let header_text = headers
            .into_iter()
            .map(|(name, value)| format!("{name}:{value}"))
            .collect::<Vec<_>>()
            .join("\n");
        let frame = FingerprintInput {
            method: &input.method,
            endpoint: &input.endpoint,
            headers: &header_text,
            body: input.body.as_bytes(),
            failure_signature: &signature,
            canonical_upstream: &canonical,
        };
        let fingerprint = key.fingerprint(&frame)?;
        let case_group = key.case_group(&frame)?;
        let reasons = input.upstream.reasons(client);
        let state = if reasons.is_empty() {
            model::CaptureState::Ready
        } else {
            model::CaptureState::Incomplete
        };
        if let Some(row) = self.replay_row(&schema::dedupe_key(&fingerprint)).await? {
            return self
                .replay_duplicate(
                    input,
                    key,
                    DuplicateCapture {
                        fingerprint: &fingerprint,
                        dedupe: &row,
                        index,
                        at,
                        limits,
                        state,
                    },
                )
                .await;
        }
        let capture_status = if state == model::CaptureState::Ready {
            "ready"
        } else {
            "incomplete"
        };
        let metadata = Metadata {
            version: 3,
            captured_at_ms: at,
            endpoint: &input.endpoint,
            method: &input.method,
            content_type: &input.content_type,
            compatibility_headers: &input.compatibility_headers,
            failure_signature: &signature,
            observation,
            client_observation: client,
            request_id: &input.request_id,
            git_sha: &input.build.git_sha,
            deno_revision: &input.build.deployment_id,
            upstream: &input.upstream,
            settings: settings(input, observation),
            capture_status,
            replay_coverage: input.upstream.coverage(client, &reasons),
            unavailable: reasons,
            body_sha256: sha256_hex(input.body.as_bytes()),
            body_bytes: input.body.as_bytes().len(),
            downstream: Downstream {
                terminal_type: &client.terminal_type,
                failure_kind: &client.failure_kind,
                error_code: &client.error_code,
                error_param: &client.error_param,
                body_base64: &client.terminal_body_base64,
                body_truncated: client.terminal_body_truncated,
            },
        };
        let metadata = Zeroizing::new(SourceJson::from_serializable(&metadata)?.into_bytes());
        if metadata.len() > replay_crypto::MAX_METADATA_BYTES {
            return Err(data::error("Replay metadata is too large"));
        }
        let capture_id = data::uuid(self.host)?;
        let iv: [u8; 12] = self
            .host
            .random_bytes(12)?
            .try_into()
            .map_err(|_| data::error("Replay IV must be 12 bytes"))?;
        let reservation = match self
            .retention()
            .reserve_replay(
                &capture_id,
                &input.request_id,
                &fingerprint,
                (
                    4 + metadata.len() + input.body.as_bytes().len(),
                    metadata.len(),
                ),
                at,
                limits,
            )
            .await?
        {
            Admission::Refused(reason) => return Ok(PersistResult::Incomplete { reason }),
            Admission::Reserved(reservation) => reservation,
        };
        let result = async {
            let mut encoded = Zeroizing::new(Vec::with_capacity(
                4 + metadata.len() + input.body.as_bytes().len(),
            ));
            encoded.extend_from_slice(&(metadata.len() as u32).to_be_bytes());
            encoded.extend_from_slice(&metadata);
            encoded.extend_from_slice(input.body.as_bytes());
            let ciphertext = key.encrypt(&iv, &fingerprint, &encoded)?;
            let manifest = model::Manifest {
                version: 1.0,
                capture_id,
                fingerprint: fingerprint.clone(),
                case_group_digest: case_group,
                captured_at_ms: at as f64,
                expires_at_ms: (at + model::PAYLOAD_TTL) as f64,
                algorithm: "AES-256-GCM".into(),
                compression: "gzip".into(),
                iv: URL_SAFE_NO_PAD.encode(iv),
                chunk_count: ciphertext.len().div_ceil(replay_crypto::CHUNK_BYTES) as f64,
                ciphertext_bytes: ciphertext.len() as f64,
                stored_bytes: Some(schema::charge(ciphertext.len(), metadata.len())),
                request_id: Some(input.request_id.clone()),
            };
            if !manifest.validate() {
                return Err(data::error("Produced replay manifest is invalid"));
            }
            let manifest_key = model::manifest_key(at as f64, &fingerprint, &manifest.capture_id)?;
            let status = model::Status {
                version: 1.0,
                request_id: input.request_id.clone(),
                status: state,
                reason: None,
                captured_at_ms: at as f64,
                manifest_key: Some(
                    data::parts(&manifest_key)?
                        .iter()
                        .map(SourceJson::from_serializable)
                        .collect::<Result<_, _>>()?,
                ),
                fingerprint: Some(fingerprint.clone()),
                expires_at_ms: Some(manifest.expires_at_ms),
            };
            let digest = sha256_hex(&ciphertext);
            let binding = index.map(|fingerprint| {
                gateway_observability_replay_retention::index::NewCaptureIndex {
                    fingerprint,
                    git_sha: &input.build.git_sha,
                    ciphertext_sha256: &digest,
                }
            });
            if !self
                .retention()
                .stage_replay(&reservation, &ciphertext, limits)
                .await?
                || !self
                    .retention()
                    .publish_replay(&reservation, &manifest, &status, limits, binding.as_ref())
                    .await?
            {
                return Ok(PersistResult::Incomplete {
                    reason: "storage_full",
                });
            }
            Ok(PersistResult::Stored {
                manifest,
                manifest_key,
            })
        }
        .await;
        if !matches!(result, Ok(PersistResult::Stored { .. })) {
            let _ = self.retention().abandon_replay(&reservation, limits).await;
        }
        if matches!(result, Ok(PersistResult::Incomplete { .. }))
            && let Some(row) = self.replay_row(&schema::dedupe_key(&fingerprint)).await?
        {
            return self
                .replay_duplicate(
                    input,
                    key,
                    DuplicateCapture {
                        fingerprint: &fingerprint,
                        dedupe: &row,
                        index,
                        at,
                        limits,
                        state,
                    },
                )
                .await;
        }
        result
    }
    async fn replay_duplicate(
        &self,
        input: &ReplayInput,
        key: &ReplayKey,
        context: DuplicateCapture<'_>,
    ) -> Result<PersistResult, GatewayError> {
        let DuplicateCapture {
            fingerprint,
            dedupe,
            index,
            at,
            limits,
            state,
        } = context;
        let row = SourceObject::parse(&read_json_record(dedupe)?)?;
        let parts = row
            .get("manifest_key")
            .ok_or_else(|| data::error("Replay dedupe record is invalid"))?
            .array_items()?;
        if parts.len() != 7 {
            return Err(data::error("Replay dedupe key is invalid"));
        }
        let manifest_key = model::tuple_from_source(&parts)?;
        let winner = self.replay_row(&manifest_key).await?;
        let manifest = winner
            .as_ref()
            .map(|r| model::Manifest::from_source(&read_json_record(r)?))
            .transpose()?;
        if manifest.as_ref().is_some_and(|m| {
            m.fingerprint != fingerprint || !m.matches_key(&manifest_key).unwrap_or(false)
        }) {
            return Err(data::error("Replay dedupe winner identity is invalid"));
        }
        let captured = row
            .get("captured_at_ms")
            .and_then(|v| v.deserialize::<f64>().ok())
            .filter(|n| model::safe_integer(*n))
            .or_else(|| manifest.as_ref().map(|m| m.captured_at_ms))
            .unwrap_or(at as f64);
        let expires = row
            .get("expires_at_ms")
            .and_then(|v| v.deserialize::<f64>().ok())
            .filter(|n| model::safe_integer(*n))
            .or_else(|| manifest.as_ref().map(|m| m.expires_at_ms))
            .unwrap_or((at + model::PAYLOAD_TTL) as f64);
        if let Some(index) = index {
            let manifest = manifest
                .as_ref()
                .ok_or_else(|| data::error("Incident replay winner is unavailable"))?;
            let exported = self.reader().export_manifest(manifest).await?;
            let mut ciphertext = Zeroizing::new(Vec::new());
            for chunk in exported.chunks {
                ciphertext.extend(
                    URL_SAFE_NO_PAD
                        .decode(chunk)
                        .map_err(|_| data::error("Replay ciphertext chunk is invalid"))?,
                )
            }
            let iv: Vec<u8> = model::url_decode(&manifest.iv)?;
            let iv: [u8; 12] = iv
                .try_into()
                .map_err(|_| data::error("Replay IV is invalid"))?;
            let plain = key.decrypt(&iv, fingerprint, &ciphertext)?;
            if plain.len() < 4 {
                return Err(data::error("Replay winner plaintext is truncated"));
            }
            let length = u32::from_be_bytes(plain[..4].try_into().expect("four bytes")) as usize;
            let bytes = plain
                .get(4..4 + length)
                .ok_or_else(|| data::error("Replay winner metadata is truncated"))?;
            let metadata = SourceObject::parse(&SourceJson::parse_bytes(bytes)?)?;
            let git_sha = metadata
                .get("git_sha")
                .and_then(|v| v.deserialize::<String>().ok());
            self.incidents()
                .bind_incident_evidence(&IncidentEvidence {
                    index_fingerprint: index,
                    reference_fingerprint: fingerprint,
                    manifest_key: &manifest_key,
                    manifest_record: winner.as_ref().expect("manifest owner"),
                    capture_id: &manifest.capture_id,
                    git_sha: git_sha.as_deref(),
                    captured_at_ms: manifest.captured_at_ms as i64,
                    expires_at_ms: manifest.expires_at_ms as i64,
                    ciphertext_sha256: &sha256_hex(&ciphertext),
                    observed_at_ms: at,
                })
                .await?;
        }
        let status = model::Status {
            version: 1.0,
            request_id: input.request_id.clone(),
            status: state,
            reason: None,
            captured_at_ms: captured,
            manifest_key: Some(parts),
            fingerprint: Some(fingerprint.into()),
            expires_at_ms: Some(expires),
        };
        let _ = self
            .replay_state()
            .admit_replay_status(&status, limits)
            .await?;
        Ok(PersistResult::Duplicate {
            fingerprint: fingerprint.into(),
            manifest_key,
        })
    }
    async fn replay_row(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        let row = self.store.read(key).await?;
        let at = data::now(self.host)?;
        Ok(row.filter(|r| r.expires_at_ms.is_none_or(|t| t > at)))
    }
}
