//! Retained v3 terminal consistency only, never current authorization or a lease.
use super::DiskMap;
use gateway_auth_native_contracts::native_import::NativeImportKind;
use gateway_auth_native_protocol::native_import::publication::data::ImportPublicationDataV1;
use gateway_core::source_control::decode_source_control_record;
use gateway_core::state::StoredRecord;
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_migration_data::{invalid, parts};
use gateway_source::error::GatewayError;
use gateway_source::source_serialization as source;
use serde_json::{Value, json};

fn value(row: &StoredRecord) -> Result<Value, GatewayError> {
    decode_source_control_record(row)?.deserialize()
}

fn row(all: &DiskMap, kind: &str, reference: &str) -> Result<StoredRecord, GatewayError> {
    all.get(
        &TupleKey::from_scalar_parts(&[
            ScalarKeyPart::String("uos_ai"),
            ScalarKeyPart::String("authority"),
            ScalarKeyPart::String("native_owner_renewal"),
            ScalarKeyPart::String("v1"),
            ScalarKeyPart::String(kind),
            ScalarKeyPart::String(reference),
        ])
        .to_record_key()
        .0,
    )?
    .ok_or_else(|| invalid("Retained v3 import terminal is missing a matching control row"))
}
fn generation_key(action: &Value) -> Result<RecordKey, GatewayError> {
    let reference = gateway_auth_primitives::crypto::sha256_hex(
        &serde_json::to_vec(&(
            "uos-native-renewal-generation-v1",
            &action["facts"]["native_owner_id"],
            &action["facts"]["account_id_hash"],
            &action["facts"]["source_native_generation"],
        ))
        .map_err(|_| invalid("Native generation binding cannot be encoded"))?,
    );
    Ok(TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("authority"),
        ScalarKeyPart::String("native_owner_renewal"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("generation"),
        ScalarKeyPart::String(&reference),
    ])
    .to_record_key())
}

pub(super) fn validate(all: &DiskMap) -> Result<(), GatewayError> {
    for name in all.index.keys() {
        let path = parts(&RecordKey(name.clone()))?;
        if path.len() != 6
            || path[..5]
                != json!([
                    "uos_ai",
                    "authority",
                    "native_owner_renewal",
                    "v1",
                    "action"
                ])
                .as_array()
                .expect("fixed array")[..]
        {
            continue;
        }
        let action_record = all.required(name)?;
        let action_record = &action_record;
        let document = decode_source_control_record(action_record)?;
        let kind = source::member(&document, "kind")
            .map(|value| value.deserialize::<NativeImportKind>())
            .transpose()?;
        let version = source::member(&document, "v")
            .map(|value| value.deserialize::<u8>())
            .transpose()?;
        if kind != Some(NativeImportKind::CodexAuth) || version != Some(3) {
            continue;
        }
        let action: Value = document.deserialize()?;
        let operation_id = action["metadata"]["operation_id"]
            .as_str()
            .ok_or_else(|| invalid("Import operation reference is missing"))?;
        if action["publication"].is_null() {
            // Preparation has no operation yet. Retained v1/v2 and unfinished
            // v3 do not acquire a new complete-triple requirement here.
            if let Ok(operation) = row(all, "operation", operation_id) {
                let operation = value(&operation)?;
                if operation["phase"] == "published"
                    || !operation["terminal"].is_null()
                    || !operation["publication_revision"].is_null()
                {
                    return Err(invalid("Unfinished v3 import has a published operation"));
                }
            }
            if let Some(generation) = all.get(&generation_key(&action)?.0)? {
                let generation = value(&generation)?;
                if generation["operation_id"] == operation_id
                    && (!generation["terminal"].is_null()
                        || !generation["publication_revision"].is_null())
                {
                    return Err(invalid("Unfinished v3 import has a published generation"));
                }
            }
            continue;
        }
        let publication: ImportPublicationDataV1 =
            serde_json::from_value(action["publication"].clone())
                .map_err(|_| invalid("Retained import publication data is malformed"))?;
        publication.validate()?;
        let operation_record = row(all, "operation", operation_id)?;
        let operation = value(&operation_record)?;
        // This is the existing native generation-key domain, not a new family.
        let generation_record = all
            .get(&generation_key(&action)?.0)?
            .ok_or_else(|| invalid("Retained v3 import terminal is missing its generation"))?;
        let generation = value(&generation_record)?;
        let terminal = json!({
            "publication_revision": publication.publication_revision,
            "source_revision": publication.source_revision,
            "normalized_revision": publication.normalized_revision,
            "native_source_generation": publication.native_source_generation,
            "access_generation": publication.access_generation,
            "credential_version": publication.credential_version,
            "publication_request_digest": publication.publication_request_digest,
        });
        if publication.operation_id != operation_id
            || operation["operation_id"] != publication.operation_id
            || operation["action_ref"] != action["metadata"]["action_ref"]
            || operation["claim_ref"] != publication.claim_ref
            || operation["phase"] != "published"
            || operation["publication_revision"] != publication.publication_revision
            || operation["native_source_generation"] != publication.native_source_generation
            || operation["access_generation"] != publication.access_generation
            || operation["terminal"] != terminal
            || generation["operation_id"] != publication.operation_id
            || generation["claim_ref"] != publication.claim_ref
            || generation["publication_revision"] != publication.publication_revision
            || generation["terminal"] != terminal
            || [action_record, &operation_record, &generation_record]
                .iter()
                .any(|record| record.revision != publication.publication_revision)
        {
            return Err(invalid(
                "Retained v3 import action and terminal pair disagree",
            ));
        }
    }
    Ok(())
}
