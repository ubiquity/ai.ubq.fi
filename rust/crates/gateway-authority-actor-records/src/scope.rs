//! Required operation scope DATA, never a cached central role.
use serde::{Deserialize, Serialize};
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum NativeActorScope {
    Client,
    Admin,
}
