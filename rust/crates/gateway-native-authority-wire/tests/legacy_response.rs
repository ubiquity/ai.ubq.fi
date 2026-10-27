use gateway_contracts::model::ModelId;
use gateway_native_authority_wire::context::NativeRenewalContext;
use gateway_native_authority_wire::{
    renewal::{NativeRenewalFence, NativeRenewalMode},
    renewal_response::NativeRenewalAuthorityResponse as Response,
};
use gateway_source::js_string::JsString16;

fn fence() -> NativeRenewalFence {
    NativeRenewalFence {
        v: 2,
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        owner_revision: "revision".into(),
        native_owner_id: "native-owner".into(),
        source_revision: "source-r1".into(),
        normalized_revision: "normalized-r1".into(),
        account_id_hash: "hash".into(),
        credential_version: "opaque-access".into(),
        source_native_generation: "opaque-native".into(),
        access_generation: "opaque-access".into(),
        access_expires_at_ms: None,
        native_refreshed_at: None,
        principal: "principal".into(),
        principal_policy_digest: "digest".into(),
        original_request_id: "request".into(),
        context: NativeRenewalContext::LiveMapped {
            call_id: "call".into(),
            mapping_revision: "mapping".into(),
        },
    }
}
#[test]
fn actual_fence_decodes_direct_and_in_original_response_with_source_model_tokens() {
    for units in [
        vec![0xd800],
        vec![0xdc00],
        vec![0xfffd],
        vec![0xd83d, 0xde80],
        vec![0, 10, 34, 92],
        r"\ud800".encode_utf16().collect(),
        vec![u16::from(b'm'); 4097],
    ] {
        let mut expected = fence();
        expected.context = NativeRenewalContext::Inference {
            attempt_id: "attempt".into(),
            original_admission_digest: "admission".into(),
            model: ModelId::from_source(JsString16::from_units(units)),
            slot: 1,
        };
        let bytes = serde_json::to_vec(&expected).unwrap();
        assert_eq!(
            serde_json::from_slice::<NativeRenewalFence>(&bytes).unwrap(),
            expected
        );
        for body in [
            format!(
                r#"{{"operation":"recheck","v":1,"fence":{}}}"#,
                std::str::from_utf8(&bytes).unwrap()
            ),
            format!(
                r#"{{"fence":{},"v":1,"operation":"recheck"}}"#,
                std::str::from_utf8(&bytes).unwrap()
            ),
        ] {
            let Response::Recheck { v: 1, fence } = serde_json::from_str(&body).unwrap() else {
                panic!("original recheck response");
            };
            assert_eq!(fence, expected);
            assert_eq!(
                serde_json::to_vec(&Response::Recheck { v: 1, fence }).unwrap(),
                [
                    br#"{"operation":"recheck","v":1,"fence":"#.as_slice(),
                    &bytes,
                    b"}"
                ]
                .concat()
            );
        }
    }
}
#[test]
fn response_variants_and_nullable_claim_fields_keep_exact_serialization() {
    for body in [
        r#"{"operation":"start","v":1,"mode":"renew_once"}"#,
        r#"{"operation":"published","v":1,"publication_revision":"r1"}"#,
        r#"{"operation":"unknown","v":1}"#,
    ] {
        let response: Response = serde_json::from_str(body).unwrap();
        assert_eq!(serde_json::to_string(&response).unwrap(), body);
    }
    let raw = serde_json::to_string(&fence()).unwrap();
    let body = format!(
        r#"{{"operation":"claim","v":1,"action_ref":"action","operation_id":"operation","claim_ref":"claim","start_permit":null,"publication_revision":null,"mode":"reconcile_only","deadline_ms":123,"fence":{raw}}}"#
    );
    let response: Response = serde_json::from_str(&body).unwrap();
    assert_eq!(serde_json::to_string(&response).unwrap(), body);
    // Existing Option fields accept omission as well as an explicit null.
    let absent = body
        .replace(",\"start_permit\":null", "")
        .replace(",\"publication_revision\":null", "");
    assert!(matches!(
        serde_json::from_str::<Response>(&absent).unwrap(),
        Response::Claim {
            start_permit: None,
            publication_revision: None,
            ..
        }
    ));
}
#[test]
fn response_missing_duplicate_unknown_cross_variant_null_and_bad_fence_versions_refuse() {
    let raw = serde_json::to_string(&fence()).unwrap();
    let valid = format!(r#"{{"operation":"recheck","v":1,"fence":{raw}}}"#);
    for body in [
        "null".into(),
        "[]".into(),
        r#"{"operation":"unknown"}"#.into(),
        r#"{"v":1}"#.into(),
        r#"{"operation":"other","v":1}"#.into(),
        r#"{"operation":"unknown","operation":"unknown","v":1}"#.into(),
        r#"{"operation":"unknown","v":1,"v":1}"#.into(),
        r#"{"operation":"unknown","v":null}"#.into(),
        r#"{"operation":"unknown","v":256}"#.into(),
        r#"{"operation":"unknown","v":1,"extra":null}"#.into(),
        r#"{"operation":"unknown","v":1,"start_permit":null}"#.into(),
        r#"{"operation":"start","v":1,"mode":null}"#.into(),
        r#"{"operation":"start","v":1}"#.into(),
        r#"{"operation":"recheck","v":1,"fence":null}"#.into(),
        r#"{"operation":"published","v":1,"publication_revision":null}"#.into(),
        format!(r#"{{"operation":"recheck","v":1,"fence":{raw},"fence":{raw}}}"#),
        valid.replacen("\"v\":2", "\"v\":1", 1),
        valid.replace("\"access_expires_at_ms\":null,", ""),
        valid.replace("\"native_refreshed_at\":null,", ""),
        valid.replace("\"fence\":", "\"mode\":\"renew_once\",\"fence\":"),
    ] {
        assert!(serde_json::from_str::<Response>(&body).is_err(), "{body}");
    }
}
