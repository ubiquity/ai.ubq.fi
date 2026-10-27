//! Closed DATA version pairings; validation grants no authentication, custody or effect.
use gateway_source::error::GatewayError;
pub const STORED_ACTOR_V1: u8 = 1;
pub const STORED_ACTOR_V2: u8 = 2;
pub const RENEWAL_ACTION_V2: u8 = 2;
pub const RENEWAL_ACTION_V3: u8 = 3;
pub const RENEWAL_FENCE_V2: u8 = 2;
pub const RENEWAL_FENCE_V3: u8 = 3;
pub const AUTHORITY_RESPONSE_V1: u8 = 1;
pub const AUTHORITY_RESPONSE_V2: u8 = 2;
pub const IMPORT_ACTION_V2: u8 = 2;
pub const IMPORT_ACTION_V3: u8 = 3;
pub const IMPORT_ACTION_V4: u8 = 4;
pub const LIVE_ACTION_V1: u8 = 1;
pub const LIVE_ACTION_V2: u8 = 2;
pub const LIVE_FENCE_V2: u8 = 2;
pub const LIVE_RESPONSE_V1: u8 = 1;
pub const LIVE_RESPONSE_V2: u8 = 2;
pub const ACTOR_POLICY_V2_DOMAIN: &[u8] = b"uos-native-actor-policy-v2\0";
pub const RENEWAL_ACTION_V3_DOMAIN: &str = "uos-native-renewal-action-v3";
pub const IMPORT_ACTION_V4_DOMAIN: &str = "uos-native-import-action-v4";
pub const IMPORT_EFFECT_V3_DOMAIN: &str = "uos-native-import-effect-v3";
pub const CONTROL_CHECKS_V2_DOMAIN: &str = "uos-native-control-checks-v2";
pub const LIVE_ACTION_V2_DOMAIN: &str = "uos-native-live-action-v2";
/// The one physical generation identity is deliberately unchanged across actor forms.
pub const GENERATION_V1_DOMAIN: &str = "uos-native-renewal-generation-v1";
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ActorForm {
    LegacyV1,
    SourceV2,
}
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum LiveFenceForm {
    LegacyScalar,
    SourceV2,
}
pub fn validate_renewal_pair(
    actor: u8,
    row: u8,
    fence: u8,
    response: u8,
) -> Result<ActorForm, GatewayError> {
    match (actor, row, fence, response) {
        (STORED_ACTOR_V1, RENEWAL_ACTION_V2, RENEWAL_FENCE_V2, AUTHORITY_RESPONSE_V1) => {
            Ok(ActorForm::LegacyV1)
        }
        (STORED_ACTOR_V2, RENEWAL_ACTION_V3, RENEWAL_FENCE_V3, AUTHORITY_RESPONSE_V2) => {
            Ok(ActorForm::SourceV2)
        }
        _ => Err(invalid()),
    }
}
pub fn validate_import_pair(actor: u8, row: u8) -> Result<ActorForm, GatewayError> {
    match (actor, row) {
        (STORED_ACTOR_V1, IMPORT_ACTION_V2 | IMPORT_ACTION_V3) => Ok(ActorForm::LegacyV1),
        (STORED_ACTOR_V2, IMPORT_ACTION_V4) => Ok(ActorForm::SourceV2),
        _ => Err(invalid()),
    }
}
pub fn validate_live_pair(
    actor: u8,
    row: u8,
    fence: LiveFenceForm,
    response: u8,
) -> Result<ActorForm, GatewayError> {
    match (actor, row, fence, response) {
        (STORED_ACTOR_V1, LIVE_ACTION_V1, LiveFenceForm::LegacyScalar, LIVE_RESPONSE_V1) => {
            Ok(ActorForm::LegacyV1)
        }
        (STORED_ACTOR_V2, LIVE_ACTION_V2, LiveFenceForm::SourceV2, LIVE_RESPONSE_V2) => {
            Ok(ActorForm::SourceV2)
        }
        _ => Err(invalid()),
    }
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Native actor DATA versions do not form a supported pair.",
        "native_actor_version_mismatch",
    )
}
