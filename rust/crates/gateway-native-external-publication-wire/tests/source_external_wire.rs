use gateway_auth_primitives::crypto::base64url;
use gateway_native_external_publication_wire::{
    receipt::SourceExternalPublicationReceipt, request::SourceExternalAccessRequest,
};
use serde_json::{Value, json};
fn request() -> Value {
    let payload=serde_json::to_vec(&json!({"exp":1800000600,"https://api.openai.com/auth":{"chatgpt_user_id":"native-user","chatgpt_account_id":"account"}})).unwrap();
    let access = format!("e30.{}.fixture", base64url(&payload));
    let native=gateway_auth_credentials::native_credentials::parse_native_document(&json!({"auth_mode":"chatgptAuthTokens","OPENAI_API_KEY":null,"tokens":{"account_id":"account","access_token":access,"id_token":access,"refresh_token":""}})).unwrap();
    json!({"operation":"source_external_access_publish","v":2,"action_ref":"action","operation_id":"operation","claim_ref":"claim","native_owner_id":"native","owner_revision":"owner","source_revision":"source","normalized_revision":"normalized","publication_revision":"publication","account_id_hash":native.routing_account_hash(),"credential_version":native.routing_credential_version(),"previous_native_source_generation":"a".repeat(64),"native_generation":native.generation_hash(),"document":{"account_id":"account","access_token":access,"user_id":"native-user","native_refreshed_at":null}})
}
fn parse(value: &Value) -> bool {
    SourceExternalAccessRequest::parse(&serde_json::to_vec(value).unwrap()).is_ok()
}
#[test]
fn strict_source_external_frame_keeps_required_nullable_time_and_finite_fields() {
    let original = request();
    assert!(parse(&original));
    for field in [
        "action_ref",
        "operation_id",
        "claim_ref",
        "native_owner_id",
        "owner_revision",
        "source_revision",
        "normalized_revision",
        "publication_revision",
        "account_id_hash",
        "credential_version",
        "previous_native_source_generation",
        "native_generation",
    ] {
        let mut value = original.clone();
        value.as_object_mut().unwrap().remove(field);
        assert!(!parse(&value), "{field}");
        value = original.clone();
        value[field] = Value::Null;
        assert!(!parse(&value), "null {field}");
    }
    let mut value = original.clone();
    value["document"]
        .as_object_mut()
        .unwrap()
        .remove("native_refreshed_at");
    assert!(!parse(&value));
    value = original;
    value["document"]["native_refreshed_at"] = json!("2026-10-29T00:00:00Z");
    assert!(parse(&value));
}
#[test]
fn source_frame_rejects_legacy_managed_mixed_unknown_and_duplicate_members() {
    let original = request();
    for operation in ["access_publish", "managed_access_publish", "config_publish"] {
        let mut value = original.clone();
        value["operation"] = json!(operation);
        assert!(!parse(&value));
    }
    for version in [0, 1, 3] {
        let mut value = original.clone();
        value["v"] = json!(version);
        assert!(!parse(&value));
    }
    for (key, value) in [
        ("unknown", json!(true)),
        ("refresh_token", json!("secret")),
        ("principal", json!("forged")),
    ] {
        let mut frame = original.clone();
        frame[key] = value;
        assert!(!parse(&frame));
    }
    let bytes = serde_json::to_string(&original).unwrap();
    let duplicate = bytes.replacen('{', "{\"v\":2,", 1);
    assert!(SourceExternalAccessRequest::parse(duplicate.as_bytes()).is_err());
    let mut value = original;
    value["document"]["refresh_token"] = json!("");
    assert!(!parse(&value));
}
#[test]
fn external_document_identity_and_derived_hashes_are_revalidated() {
    let original = request();
    for field in ["account_id_hash", "credential_version", "native_generation"] {
        let mut value = original.clone();
        value[field] = json!("b".repeat(64));
        assert!(!parse(&value), "{field}");
    }
    for field in ["account_id", "user_id"] {
        let mut value = original.clone();
        value["document"][field] = json!("foreign");
        assert!(!parse(&value), "{field}");
    }
    let mut value = original.clone();
    value["document"]["access_token"] = json!("not-a-jwt");
    assert!(!parse(&value));
    for bad in ["", "non ascii 😀", " space "] {
        let mut value = original.clone();
        value["claim_ref"] = json!(bad);
        assert!(!parse(&value));
    }
}
#[test]
fn receipt_is_strict_and_does_not_collapse_acknowledged_and_current_revisions() {
    let request = request();
    let mut value = request.clone();
    let fields = value.as_object_mut().unwrap();
    fields.remove("document");
    fields.insert("owner_id".into(), json!("owner"));
    fields.insert("owner_epoch".into(), json!("epoch"));
    fields.insert("disposition".into(), json!("already_current"));
    fields.insert("current_source_revision".into(), json!("older-source"));
    fields.insert(
        "current_normalized_revision".into(),
        json!("older-normalized"),
    );
    let receipt: SourceExternalPublicationReceipt = serde_json::from_value(value.clone()).unwrap();
    assert!(receipt.matches(
        &SourceExternalAccessRequest::parse(&serde_json::to_vec(&request).unwrap()).unwrap()
    ));
    assert_eq!(receipt.publication_revision, "publication");
    assert_eq!(receipt.current_source_revision, "older-source");
    for field in [
        "action_ref",
        "operation_id",
        "claim_ref",
        "native_owner_id",
        "owner_revision",
        "source_revision",
        "normalized_revision",
        "publication_revision",
        "account_id_hash",
        "credential_version",
        "previous_native_source_generation",
        "native_generation",
    ] {
        let mut changed = value.clone();
        changed[field] = json!("foreign");
        let receipt: SourceExternalPublicationReceipt = serde_json::from_value(changed).unwrap();
        assert!(
            !receipt.matches(
                &SourceExternalAccessRequest::parse(&serde_json::to_vec(&request).unwrap())
                    .unwrap()
            ),
            "{field}"
        );
    }
    value["unknown"] = json!(true);
    assert!(serde_json::from_value::<SourceExternalPublicationReceipt>(value).is_err());
}
#[test]
fn every_declared_native_reference_retains_the_colon_domain() {
    let original = request();
    for field in [
        "action_ref",
        "operation_id",
        "claim_ref",
        "native_owner_id",
        "owner_revision",
        "source_revision",
        "normalized_revision",
        "publication_revision",
    ] {
        let mut value = original.clone();
        value[field] = json!(format!("native:{field}"));
        assert!(parse(&value), "colon reference {field}");
        value[field] = json!("x".repeat(257));
        assert!(!parse(&value), "oversized reference {field}");
    }
}
#[test]
fn complete_source_v2_request_and_receipt_golden_bytes_keep_distinct_revisions() {
    let value = request();
    let parsed = SourceExternalAccessRequest::parse(&serde_json::to_vec(&value).unwrap()).unwrap();
    let expected = format!(
        r#"{{"operation":"source_external_access_publish","v":2,"action_ref":"action","operation_id":"operation","claim_ref":"claim","native_owner_id":"native","owner_revision":"owner","source_revision":"source","normalized_revision":"normalized","publication_revision":"publication","account_id_hash":"{}","credential_version":"{}","previous_native_source_generation":"{}","native_generation":"{}","document":{{"account_id":"account","access_token":"{}","user_id":"native-user","native_refreshed_at":null}}}}"#,
        value["account_id_hash"].as_str().unwrap(),
        value["credential_version"].as_str().unwrap(),
        "a".repeat(64),
        value["native_generation"].as_str().unwrap(),
        value["document"]["access_token"].as_str().unwrap()
    );
    assert_eq!(serde_json::to_vec(&parsed).unwrap(), expected.as_bytes());
    let receipt=SourceExternalPublicationReceipt{v:2,operation:"source_external_access_publish".into(),disposition:gateway_native_external_publication_wire::receipt::SourceExternalPublicationDisposition::AlreadyCurrent,owner_id:"owner".into(),owner_epoch:"epoch".into(),action_ref:"action".into(),operation_id:"operation".into(),claim_ref:"claim".into(),native_owner_id:"native".into(),owner_revision:"owner".into(),source_revision:"source".into(),normalized_revision:"normalized".into(),publication_revision:"publication".into(),current_source_revision:"older-source".into(),current_normalized_revision:"older-normalized".into(),account_id_hash:parsed.account_id_hash.clone(),credential_version:parsed.credential_version.clone(),previous_native_source_generation:"a".repeat(64),native_generation:parsed.native_generation.clone()};
    let expected = format!(
        r#"{{"v":2,"operation":"source_external_access_publish","disposition":"already_current","owner_id":"owner","owner_epoch":"epoch","action_ref":"action","operation_id":"operation","claim_ref":"claim","native_owner_id":"native","owner_revision":"owner","source_revision":"source","normalized_revision":"normalized","publication_revision":"publication","current_source_revision":"older-source","current_normalized_revision":"older-normalized","account_id_hash":"{}","credential_version":"{}","previous_native_source_generation":"{}","native_generation":"{}"}}"#,
        parsed.account_id_hash,
        parsed.credential_version,
        "a".repeat(64),
        parsed.native_generation
    );
    assert_eq!(serde_json::to_vec(&receipt).unwrap(), expected.as_bytes());
    assert!(receipt.matches(&parsed));
}
