//! Source incident-outbox.ts: durable safe classifications, never raw request data.
use crate::{ObservabilityRuntime, store};
use gateway_admin_runtime::{capabilities::CaptureRequest, capabilities::Context};
use gateway_auth_contracts::types::AuthResponse;
use gateway_core::{
    query::QueryRequest, query::QuerySelector, query::QueryStore, state::RecordKey,
    state::StoredRecord,
};
use gateway_observability_contracts::{
    data, source_object::SourceObject, types::ObservabilityHost,
};
use gateway_observability_replay_model::{
    records::read_json_record, replay_model, replay_model as model,
};
use gateway_observability_replay_records::{
    incident::ContextMessage, incident::EvidenceRef, incident::IncidentIndexData,
    incident::IncidentObservation, incident::MESSAGE, incident::Provenance, incident::endpoint,
    incident::full_sha, incident::index_key, incident::valid_incident_id,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::json;

fn prefix() -> RecordKey {
    data::key(json!(["uos_ai", "sentinel_incident", "v1", "index"]))
}

pub struct IncidentIndexReceipt {
    pub incident_id: String,
    pub fingerprint: String,
    pub key: RecordKey,
    pub record: StoredRecord,
}
pub struct IncidentEvidence<'a> {
    pub index_fingerprint: &'a str,
    pub reference_fingerprint: &'a str,
    pub manifest_key: &'a RecordKey,
    pub manifest_record: &'a StoredRecord,
    pub capture_id: &'a str,
    pub git_sha: Option<&'a str>,
    pub captured_at_ms: i64,
    pub expires_at_ms: i64,
    pub ciphertext_sha256: &'a str,
    pub observed_at_ms: i64,
}
impl<S: QueryStore, H: ObservabilityHost> ObservabilityRuntime<S, H> {
    pub async fn record_incident_index(
        &self,
        input: &IncidentObservation,
    ) -> Result<IncidentIndexReceipt, GatewayError> {
        if input.observed_at_ms <= 0
            || input.observed_at_ms > gateway_core::identity::MAX_SAFE_INTEGER
        {
            return Err(data::error("Sentinel incident timestamp is invalid"));
        }
        let fingerprint = input.fingerprint()?;
        let key = index_key(&fingerprint);
        let latest = input.latest();
        for _ in 0..8 {
            let previous = self.store.read(&key).await?;
            let (mut document, mut data) = if let Some(row) = previous.as_ref() {
                let source = read_json_record(row)?;
                (
                    SourceObject::parse(&source)?,
                    IncidentIndexData::parse(&source)?,
                )
            } else {
                let incident_id = format!("provider-{}", data::uuid(self.host.as_ref())?);
                let data = IncidentIndexData {
                    version: 1.0,
                    incident_id,
                    fingerprint: fingerprint.clone(),
                    severity: "P2".into(),
                    first_seen_at_ms: input.observed_at_ms as f64,
                    last_seen_at_ms: input.observed_at_ms as f64,
                    count: 0.0,
                    failing_revision: full_sha(&input.git_sha).then(|| input.git_sha.clone()),
                    error_type: input.terminal(),
                    context: ContextMessage {
                        message: MESSAGE.into(),
                        location: (),
                        sample: Vec::new(),
                    },
                    provenance: Provenance {
                        endpoint: endpoint(&input.endpoint).into(),
                        captured_at_ms: input.observed_at_ms as f64,
                        captured_by: (),
                    },
                    evidence_ref: None,
                    evidence_expires_at_ms: None,
                    latest: Some(latest.clone()),
                };
                (
                    SourceObject::parse(&SourceJson::from_serializable(&data)?)?,
                    data,
                )
            };
            let newest = data.last_seen_at_ms <= input.observed_at_ms as f64;
            data.first_seen_at_ms = data.first_seen_at_ms.min(input.observed_at_ms as f64);
            data.last_seen_at_ms = data.last_seen_at_ms.max(input.observed_at_ms as f64);
            data.count += 1.0;
            if newest {
                data.latest = Some(latest.clone());
                document.set("latest", &data.latest)?;
            }
            if !data.valid() {
                return Err(data::error("Sentinel incident index record is invalid"));
            }
            for (name, value) in [
                ("first_seen_at_ms", data.first_seen_at_ms),
                ("last_seen_at_ms", data.last_seen_at_ms),
                ("count", data.count),
            ] {
                document.set(name, &value)?
            }
            let mutation = crate::replay_values::put_json(
                self.host.as_ref(),
                key.clone(),
                &document.source_json()?,
                None,
            )?;
            let gateway_core::state::RecordMutation::Put { record, .. } = mutation else {
                unreachable!()
            };
            if store::commit(
                self.store.as_ref(),
                vec![data::check(&key, previous.as_ref())],
                vec![gateway_core::state::RecordMutation::Put {
                    key: key.clone(),
                    record: record.clone(),
                }],
            )
            .await?
            {
                return Ok(IncidentIndexReceipt {
                    incident_id: data.incident_id,
                    fingerprint,
                    key,
                    record,
                });
            }
        }
        Err(data::error(
            "Sentinel incident index update conflicted repeatedly",
        ))
    }
    pub async fn list_incidents(
        &self,
        id: Option<&str>,
        cursor: Option<&str>,
        limit: u32,
    ) -> Result<AuthResponse, GatewayError> {
        if !(1..=100).contains(&limit) || cursor.is_some_and(|s| !model::cursor_shape(s)) {
            return Err(data::invalid(
                "Sentinel incident index pagination is invalid",
            ));
        }
        let result = async {
            let query = QueryRequest {
                selector: QuerySelector::Prefix {
                    prefix: prefix(),
                    start: None,
                    end: None,
                },
                reverse: false,
                limit,
                cursor: cursor.map(model::core_cursor).transpose()?,
                now_ms: data::now(self.host.as_ref())?,
            };
            let page = self.store.query(query).await?;
            let mut rows = Vec::new();
            for entry in page.entries {
                let source = read_json_record(&entry.record)?;
                let row = IncidentIndexData::parse(&source)?;
                if key(&row.fingerprint) != entry.key {
                    return Err(data::error("Sentinel incident index key is invalid"));
                }
                if id.is_none_or(|id| row.incident_id == id) {
                    rows.push(source)
                }
            }
            #[derive(Serialize)]
            struct Page {
                data: Vec<SourceJson>,
                cursor: Option<String>,
            }
            Ok(AuthResponse::source_json(
                200,
                SourceJson::from_serializable(&Page {
                    data: rows,
                    cursor: page
                        .next_cursor
                        .as_deref()
                        .map(model::wire_cursor)
                        .transpose()?,
                })?,
            ))
        }
        .await;
        result.map_err(|_| {
            GatewayError::unavailable(
                "Sentinel incidents unavailable",
                "sentinel_incidents_unavailable",
            )
        })
    }
    pub async fn bind_incident_evidence(
        &self,
        input: &IncidentEvidence<'_>,
    ) -> Result<(), GatewayError> {
        if !model::digest(input.index_fingerprint)
            || !model::digest(input.reference_fingerprint)
            || !model::digest(input.ciphertext_sha256)
            || input.observed_at_ms <= 0
            || input.captured_at_ms <= 0
            || input.expires_at_ms <= 0
            || !model::capture_id(input.capture_id)
        {
            return Err(data::error(
                "Sentinel incident index evidence binding is invalid",
            ));
        }
        let key = key(input.index_fingerprint);
        for _ in 0..8 {
            let previous = self
                .store
                .read(&key)
                .await?
                .ok_or_else(|| data::error("Sentinel incident index record is missing"))?;
            let source = read_json_record(&previous)?;
            let row = IncidentIndexData::parse(&source)?;
            let mut document = SourceObject::parse(&source)?;
            document.set("failing_revision", &input.git_sha.filter(|s| full_sha(s)))?;
            let mut provenance = SourceObject::parse(
                document
                    .get("provenance")
                    .ok_or_else(|| data::error("Incident provenance missing"))?,
            )?;
            provenance.set("captured_at_ms", &input.captured_at_ms)?;
            document.set("provenance", &provenance.source_json()?)?;
            document.set(
                "evidence_ref",
                &EvidenceRef {
                    r#ref: format!("capture:{}", input.capture_id),
                    digest: Some(input.ciphertext_sha256.into()),
                },
            )?;
            document.set("evidence_expires_at_ms", &input.expires_at_ms)?;
            let document = document.source_json()?;
            IncidentIndexData::parse(&document)?;
            let mut mutations = vec![crate::replay_values::put_json(
                self.host.as_ref(),
                key.clone(),
                &document,
                None,
            )?];
            if input.expires_at_ms > input.observed_at_ms {
                let parts = data::parts(input.manifest_key)?;
                mutations.push(store::put(
                    self.host.as_ref(),
                    data::key(json!([
                        "uos_ai",
                        "sentinel_incident",
                        "v1",
                        "capture_ref",
                        row.incident_id,
                        input.reference_fingerprint
                    ])),
                    json!({"version":1,"manifest_key":parts}),
                    Some(input.expires_at_ms),
                )?)
            }
            if store::commit(
                self.store.as_ref(),
                vec![
                    data::check(&key, Some(&previous)),
                    data::check(input.manifest_key, Some(input.manifest_record)),
                ],
                mutations,
            )
            .await?
            {
                return Ok(());
            }
        }
        Err(data::error(
            "Sentinel incident index evidence binding conflicted repeatedly",
        ))
    }
    pub async fn capture(
        &self,
        request: CaptureRequest,
        _context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        match request {
            CaptureRequest::Request { request_id } => {
                self.export_capture_request(&request_id).await
            }
            CaptureRequest::Interval {
                after_ms,
                before_ms,
                cursor,
            } => {
                self.export_capture_interval(after_ms, before_ms, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incident {
                incident_id,
                cursor,
            } => {
                self.export_incident_captures(&incident_id, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incidents {
                incident_id,
                cursor,
                limit,
            } => {
                self.list_incidents(incident_id.as_deref(), cursor.as_deref(), limit)
                    .await
            }
        }
    }
}
