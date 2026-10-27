//! Ordinary cache campaign records; current campaign/refresh permits remain authority-owned.
pub mod model;

use crate::model::{Sample, TargetBinding};
use serde::{Deserialize, Serialize};

/// Closed private renewal context; only the source fifth-row transition mints its permit.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RefreshContext {
    pub v: u8,
    pub release_git_sha: String,
    pub target: TargetBinding,
    pub campaign_owner: String,
    pub cycle_owner: String,
    pub cycle: u8,
    pub source_pool_revision: String,
    pub slot: u8,
    pub state_revision: String,
    pub cycle_lease_revision: String,
    pub campaign_lease_revision: String,
    pub prefix: [Sample; 5],
}
