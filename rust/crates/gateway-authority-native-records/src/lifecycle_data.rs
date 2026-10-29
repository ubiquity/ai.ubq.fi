use crate::rows::{self, fail};
use gateway_authority_contracts::{text, validate_time};
use gateway_core::state::StoredRecord;
use gateway_native_authority_wire::source_fence::SourceNativeRenewalFenceV3;
use gateway_source::error::GatewayError;
pub fn generation_key(
    fence: &SourceNativeRenewalFenceV3,
) -> Result<gateway_kv_codec::key::RecordKey, GatewayError> {
    rows::generation_key_parts(
        &fence.native_owner_id,
        &fence.account_id_hash,
        &fence.source_native_generation,
    )
}
pub fn metadata(row: &StoredRecord, now: i64) -> Result<(), GatewayError> {
    row.validate_version()?;
    text(&row.revision)?;
    if let Some(expiry) = row.expires_at_ms {
        validate_time(expiry)?;
        if expiry <= now {
            return Err(fail());
        }
    }
    Ok(())
}
