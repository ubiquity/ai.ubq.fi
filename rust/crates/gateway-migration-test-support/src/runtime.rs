//! Fixed synthetic migration data and runtime hosts, never production constructors.
use gateway_admin_contracts::operations::AdminInput;
use gateway_auth_primitives::crypto::sha256_base64url;
use gateway_kv_codec::value::PortableValue;
use gateway_migration_data::capture::PrivateCapture;
use gateway_migration_data::capture::SourceEntry;
use gateway_profile_source::codec::to_source_profile as to_profile;
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::{Value, json};
use std::sync::atomic::{AtomicU64, Ordering};
pub fn raw(value: Value) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(&value)
}
pub const SOURCE: &str = "6d4d4790a7c06514b92938ff677dbfd52764461d";
pub fn source(
    parts: Value,
    value: Value,
    revision: &str,
    expires: Option<i64>,
) -> Result<SourceEntry, GatewayError> {
    Ok(SourceEntry {
        key: parts
            .as_array()
            .ok_or_else(|| {
                GatewayError::unavailable(
                    "Synthetic tuple fixture must be an array",
                    "synthetic_fixture_invalid",
                )
            })?
            .iter()
            .map(|part| to_profile(&PortableValue::from_json(part)))
            .collect::<Result<Vec<_>, GatewayError>>()?,
        value: to_profile(&PortableValue::from_json(&value))?,
        versionstamp: revision.into(),
        expires_at_ms: expires,
    })
}
pub fn capture(entries: Vec<SourceEntry>) -> PrivateCapture {
    PrivateCapture {
        v: 1,
        source_git_sha: SOURCE.into(),
        captured_at_ms: 2200,
        entries,
    }
}
pub fn fixture() -> Result<(PrivateCapture, String), GatewayError> {
    let token = format!("u_{}", "1".repeat(64));
    let hash = sha256_base64url(token.as_bytes());
    let hash_record = json!({"id":"fixture-key","expires_at_ms":-1,"revoked_at_ms":null,"usage_limit_requests":2,"usage_requests":1,"usage_reset_at_ms":3000,"window_ms":1000,"usage_quota_version":3,"paid_fallback_enabled":false,"paid_fallback_limit_microcredits":0,"paid_fallback_spent_microcredits":0,"paid_fallback_reserved_microcredits":0,"paid_fallback_reservation_request_id":null});
    let mut id = hash_record.clone();
    for (k,v) in json!({"name":"Synthetic fixture","prefix":&token[..12],"hash":hash,"created_at_ms":1000,"paid_fallback_model_ids":[],"paid_fallback_quota_per_credit":0,"paid_fallback_max_exposure_microcredits":{},"paid_fallback_pricing_checked_at_ms":null}).as_object().ok_or_else(|| GatewayError::unavailable("Synthetic key fixture must be an object", "synthetic_fixture_invalid"))?{id[k]=v.clone();}
    let window = json!({"v":3,"key_id":"fixture-key","policy_version":"v3:1000","window_start_ms":2000,"window_reset_at_ms":3000,"committed_requests":1,"reserved_requests":1,"updated_at_ms":2100});
    let request = |id: &str, state: &str| json!({"v":3,"key_id":"fixture-key","request_id":id,"route":"responses","state":state,"reserved_at_ms":2000,"lease_expires_at_ms":2100,"provider":if state=="dispatched"{json!("deepseek")}else{Value::Null},"dispatched_at_ms":if state=="dispatched"{json!(2050)}else{Value::Null},"released_at_ms":null,"release_reason":null});
    Ok((
        capture(vec![
            source(
                json!(["ubq_ai", "api_keys", "id", "fixture-key"]),
                id,
                "id-original",
                None,
            )?,
            source(
                json!(["ubq_ai", "api_keys", "hash", hash]),
                hash_record,
                "hash-original",
                None,
            )?,
            source(
                json!([
                    "uos_ai",
                    "api_key_usage",
                    "v3",
                    "window",
                    "fixture-key",
                    "v3:1000",
                    2000
                ]),
                window,
                "window-original",
                Some(6000),
            )?,
            source(
                json!([
                    "uos_ai",
                    "api_key_usage",
                    "v3",
                    "request",
                    "fixture-key",
                    "v3:1000",
                    2000,
                    "old-reserved"
                ]),
                request("old-reserved", "reserved"),
                "reserved-original",
                Some(6000),
            )?,
            source(
                json!([
                    "uos_ai",
                    "api_key_usage",
                    "v3",
                    "request",
                    "fixture-key",
                    "v3:1000",
                    2000,
                    "old-dispatched"
                ]),
                request("old-dispatched", "dispatched"),
                "dispatched-original",
                Some(6000),
            )?,
            source(
                json!(["uos_ai", "debug_routing", "v1"]),
                json!({"scenario":"codex_429","expires_at_ms":5000,"updated_at_ms":2000}),
                "debug-original",
                Some(5000),
            )?,
        ]),
        token,
    ))
}
pub struct Tokens(pub AtomicU64);
impl gateway_authority_contracts::TokenSource for Tokens {
    fn next_token(&self) -> Result<String, GatewayError> {
        Ok(format!(
            "fixture-revision-{}",
            self.0.fetch_add(1, Ordering::SeqCst)
        ))
    }
}
pub struct Clock;
impl gateway_authority_contracts::AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(2300)
    }
}

pub fn paid_exposure_capture() -> Result<PrivateCapture, GatewayError> {
    let attempt = |id: &str, pending: bool| json!({"v":3,"key_id":"fixture-key","request_id":id,"policy_version":"paid:v3","route":"responses","path":"/v1/responses","model":"gpt-fixture","stream":false,"reasoning":null,"provider":"surplus","window_reset_at_ms":3000,"reserved_microcredits":100,"quota_per_credit":1000,"provider_request_id":if pending{Value::Null}else{json!("provider-settled")},"provider_quota":if pending{Value::Null}else{json!(0.05)},"input_tokens":if pending{Value::Null}else{json!(10)},"cached_input_tokens":null,"output_tokens":if pending{Value::Null}else{json!(20)},"dispatch_state":"dispatched","terminal_state":if pending{"ambiguous"}else{"completed"},"spend_microcredits":if pending{Value::Null}else{json!(50)},"billing_state":if pending{"unresolved"}else{"settled"},"reconciliation_attempts":0,"last_reconciliation_at_ms":null,"dispatched_at_ms":2005,"terminal_at_ms":2100,"settled_at_ms":if pending{Value::Null}else{json!(2100)},"created_at_ms":2000,"updated_at_ms":2100,"usage_rollup_at_ms":if pending{Value::Null}else{json!(2100)}});
    Ok(capture(vec![
        source(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "window",
                "fixture-key",
                3000
            ]),
            json!({"v":3,"key_id":"fixture-key","policy_version":"paid:v3","window_reset_at_ms":3000,"limit_microcredits":1000,"settled_microcredits":50,"reserved_microcredits":100,"pending_count":1,"updated_at_ms":2100}),
            "paid-window",
            Some(9000),
        )?,
        source(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "request",
                "fixture-key",
                "pending-attempt"
            ]),
            attempt("pending-attempt", true),
            "pending-attempt-source",
            Some(9000),
        )?,
        source(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "request",
                "fixture-key",
                "settled-attempt"
            ]),
            attempt("settled-attempt", false),
            "settled-attempt-source",
            Some(9000),
        )?,
        source(
            json!([
                "uos_ai",
                "paid_fallback",
                "v3",
                "pending",
                "fixture-key",
                "pending-attempt"
            ]),
            json!({"created_at_ms":2000,"next_reconciliation_at_ms":2200}),
            "pending-marker-source",
            None,
        )?,
        source(
            json!(["uos_ai", "paid_fallback", "v3", "reconciliation_gate"]),
            json!({"next_due_at_ms":2200}),
            "gate-source",
            None,
        )?,
    ]))
}

pub struct AdminHost;
impl gateway_auth_contracts::webauthn::WebAuthnCrypto for AdminHost {
    async fn validate_key(
        &self,
        _: &gateway_auth_contracts::webauthn::CosePublicKey,
    ) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "not used in fixture",
            "unused_crypto",
        ))
    }
    async fn verify_signature(
        &self,
        _: gateway_auth_contracts::webauthn::SignatureInput,
    ) -> Result<bool, GatewayError> {
        Err(GatewayError::unavailable(
            "not used in fixture",
            "unused_crypto",
        ))
    }
}
impl gateway_auth_contracts::types::AuthHost for AdminHost {
    fn now_ms(&self) -> i64 {
        2300
    }
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        Ok(vec![7; length])
    }
    async fn fetch_auth(
        &self,
        _: gateway_auth_contracts::upstream::AuthUpstreamRequest,
    ) -> Result<gateway_auth_contracts::upstream::AuthUpstreamResponse, GatewayError> {
        Err(GatewayError::unavailable(
            "no network in fixture",
            "unused_network",
        ))
    }
    async fn kernel_principal(
        &self,
        _: &str,
        _: &str,
        _: &str,
        _: &str,
    ) -> Result<String, GatewayError> {
        Err(GatewayError::unavailable("not used", "unused_kernel"))
    }
}
pub struct Capabilities;
impl gateway_admin_runtime_contracts::capabilities::RuntimeCapabilities for Capabilities {}
pub fn admin_input(body: Value) -> Result<AdminInput, GatewayError> {
    Ok(AdminInput {
        parameter: None,
        query: Default::default(),
        body: gateway_source::source_json::SourceJson::from_serializable(&body)?,
        actor: gateway_auth_contracts::types::AuthPrincipal {
            token: None,
            method: gateway_auth_contracts::types::AuthMethod::AdminAllowlist,
            is_admin: true,
            is_super_admin: false,
        },
        request_id: "isolated-admin".into(),
        now_ms: 2300,
    })
}
