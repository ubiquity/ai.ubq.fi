//! Ordinary renewal context DATA; no permits or authority.
use gateway_core::{identity::AccountBinding, model::ModelId};
use serde::Serialize;
mod codec;
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeRenewalContext {
    Inference {
        attempt_id: String,
        original_admission_digest: String,
        model: ModelId,
        slot: u64,
    },
    LiveCreate {
        binding: AccountBinding,
        active_generation: u64,
        model_utf16: Option<Vec<u16>>,
    },
    LiveMapped {
        call_id: String,
        mapping_revision: String,
    },
    CacheExperiment {
        context: Box<gateway_cache_experiment_records::RefreshContext>,
    },
}
