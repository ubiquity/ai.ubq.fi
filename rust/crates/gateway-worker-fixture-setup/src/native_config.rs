//! Fixed synthetic native fixture configuration; no store or proof is exposed.
use gateway_publication_policy::control_publication::ConfiguredPaidInputs;
use gateway_source::error::GatewayError;

pub const SURPLUS_SECRET: &str = "fixture-matrix-surplus";
pub const OPENLUX_DISCOVERY_SECRET: &str = "fixture-matrix-openlux-discovery";
pub const OPENLUX_SPEND_SECRET: &str = "fixture-matrix-openlux-spend";

pub fn now() -> i64 {
    gateway_worker_storage::store::now_ms().unwrap_or(i64::MAX)
}

pub fn inputs() -> Result<ConfiguredPaidInputs, GatewayError> {
    use gateway_auth_primitives::crypto::Secret;
    ConfiguredPaidInputs::new(
        Some(Secret::new(SURPLUS_SECRET.into())),
        Some(Secret::new(OPENLUX_DISCOVERY_SECRET.into())),
        Some(Secret::new(OPENLUX_SPEND_SECRET.into())),
        serde_json::json!({"fixture":"runtime-matrix-v1"}),
    )
}
