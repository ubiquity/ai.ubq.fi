//! Private incident-index publication is consumed only by the same retention final CAS.
use crate::{ReplayRetention, replay_values, store};
use gateway_core::{query::QueryStore, state::RecordMutation, state::RevisionCheck};
use gateway_kv_codec::key::RecordKey;
use gateway_observability_contracts::{data, types::ObservabilityHost};
use gateway_observability_replay_model::{records::read_json_record, replay_model as model};
use gateway_observability_replay_records::{incident::capture_index_document, incident::index_key};
use gateway_source::error::GatewayError;
use serde_json::json;
pub(crate) struct IndexPublication {
    pub checks: Vec<RevisionCheck>,
    pub mutations: Vec<RecordMutation>,
}
pub struct NewCaptureIndex<'a> {
    pub fingerprint: &'a str,
    pub git_sha: &'a str,
    pub ciphertext_sha256: &'a str,
}
impl<S: QueryStore, H: ObservabilityHost> ReplayRetention<'_, S, H> {
    pub(crate) async fn new_capture_index_publication(
        &self,
        binding: &NewCaptureIndex<'_>,
        manifest: &model::Manifest,
        manifest_key: &RecordKey,
    ) -> Result<Option<IndexPublication>, GatewayError> {
        if !model::digest(binding.fingerprint) || !model::digest(binding.ciphertext_sha256) {
            return Err(data::error("New replay index binding is invalid"));
        }
        let key = index_key(binding.fingerprint);
        let Some(previous) = self.store.read(&key).await? else {
            return Ok(None);
        };
        let source = read_json_record(&previous)?;
        let (document, incident_id) = capture_index_document(
            &source,
            manifest,
            binding.git_sha,
            binding.ciphertext_sha256,
        )?;
        let mut mutations = vec![replay_values::put_json(
            self.host,
            key.clone(),
            &document,
            None,
        )?];
        if manifest.expires_at_ms > data::now(self.host)? as f64 {
            mutations.push(store::put(
                self.host,
                data::key(json!([
                    "uos_ai",
                    "sentinel_incident",
                    "v1",
                    "capture_ref",
                    incident_id,
                    manifest.fingerprint
                ])),
                json!({"version":1,"manifest_key":data::parts(manifest_key)?}),
                Some(manifest.expires_at_ms as i64),
            )?)
        }
        Ok(Some(IndexPublication {
            checks: vec![data::check(&key, Some(&previous))],
            mutations,
        }))
    }
}
