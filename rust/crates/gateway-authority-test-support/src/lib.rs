use crate::control as paid_control_cases;
use crate::original as paid_original;
use gateway_authority::paid_types::PaidAuthorization;
use gateway_authority_keys as keys;
use gateway_authority_paid_records::{PaidAdmission, PaidRequest};
use gateway_core::identity::ApiKeyId;
use gateway_core::identity::PaidAttemptId;
use gateway_core::identity::RequestId;
use gateway_core::provider::PaidProvider;
use gateway_core::state::RecordKey;
use gateway_core::state::StoredRecord;
use gateway_routing_runtime_test_support::authority::MemoryStore;
use gateway_routing_runtime_test_support::authority::NOW;
use gateway_routing_runtime_test_support::authority::TestAuthority;
use gateway_routing_runtime_test_support::authority::policy;
use gateway_routing_runtime_test_support::authority::run;
use serde::de::DeserializeOwned;
pub mod control;
pub mod original;

pub type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
pub fn fixture_row(store: &MemoryStore, key: &RecordKey) -> TestResult<StoredRecord> {
    store.row(key).ok_or_else(|| "expected fixture row".into())
}
pub fn fixture_json<T: DeserializeOwned>(store: &MemoryStore, key: &RecordKey) -> TestResult<T> {
    Ok(serde_json::from_value(fixture_row(store, key)?.value)?)
}
pub fn id(value: &str) -> RequestId {
    RequestId(value.to_owned())
}
pub fn seed_policy(store: &MemoryStore) {
    store.seed(keys::api_policy("key-a"), &policy());
}
pub fn paid_setup(store: &MemoryStore) {
    seed_policy(store);
    paid_control_cases::seed_controls(store);
}
pub fn paid_request(attempt: &str, provider: PaidProvider) -> PaidRequest {
    PaidRequest {
        key_id: ApiKeyId("key-a".to_owned()),
        request_id: id("client-one"),
        attempt_id: PaidAttemptId(attempt.to_owned()),
        provider,
        model: "paid-model".into(),
        route: "responses".to_owned(),
        path: "/v1/responses".to_owned(),
        stream: true,
        reasoning: None,
        created_at_ms: NOW,
    }
}
pub async fn reserve_paid(
    authority: &TestAuthority,
    input: &PaidRequest,
    authorization: &PaidAuthorization,
    now: i64,
) -> Result<PaidAdmission, gateway_core::error::GatewayError> {
    authority.store().clock(now);
    let context = paid_control_cases::context(input);
    let receipt = paid_original::authorize(authority, input, &context, now).await?;
    authority
        .reserve_paid(input, authorization, &receipt, now)
        .await
}

pub fn paid_reservation(
    authority: &TestAuthority,
    input: &PaidRequest,
) -> TestResult<gateway_authority_paid_records::PaidReservation> {
    match run(reserve_paid(
        authority,
        input,
        &PaidAuthorization::DirectPaidModel,
        NOW,
    ))? {
        PaidAdmission::Reserved(reservation) => Ok(reservation),
        _ => Err("expected fresh reservation".into()),
    }
}
