//! Complete stored duplicate resolution, evidence binding and status admission.
use crate::ReplayReader;
use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_core::query::QueryStore;
use gateway_kv_codec::key::RecordKey;
use gateway_observability_contracts::{
    data, source_object::SourceObject, types::ObservabilityHost,
};
use gateway_observability_incidents::{IncidentIndex, incidents::IncidentEvidence};
use gateway_observability_replay_model::{
    records::read_json_record, replay_crypto::ReplayKey, replay_model as model,
};
use gateway_observability_replay_records::accounting::RetentionLimits;
use gateway_observability_replay_state::{ReplayBootstrapState, ReplayMetadata};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use zeroize::Zeroizing;

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
/// Previously observed ordinary duplicate-input DATA; not a permission or claim.
pub struct DuplicateCapture<'a> {
    pub fingerprint: &'a str,
    pub dedupe: &'a gateway_core::state::StoredRecord,
    pub index: Option<&'a str>,
    pub at: i64,
    pub limits: RetentionLimits,
    pub state: model::CaptureState,
}
pub struct ReplayDuplicates<'a, S, H> {
    store: &'a S,
    host: &'a H,
    state: ReplayBootstrapState,
}
impl<'a, S, H> ReplayDuplicates<'a, S, H> {
    pub fn new(store: &'a S, host: &'a H, state: ReplayBootstrapState) -> Self {
        Self { store, host, state }
    }
    fn reader(&self) -> ReplayReader<'_, S, H> {
        ReplayReader::new(self.store, self.host)
    }
    fn incidents(&self) -> IncidentIndex<'_, S, H> {
        IncidentIndex::new(self.store, self.host)
    }
    fn replay_state(&self) -> ReplayMetadata<'_, S, H> {
        ReplayMetadata::new(self.store, self.host, self.state.clone())
    }
}
impl<S: QueryStore, H: ObservabilityHost> ReplayDuplicates<'_, S, H> {
    pub async fn persist_duplicate(
        &self,
        request_id: &str,
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
        let manifest_key = crate::reference::tuple_from_source(&parts)?;
        let winner = self.reader().replay_row(&manifest_key).await?;
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
            request_id: request_id.to_owned(),
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
}
