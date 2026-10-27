use gateway_auth_native_proof::native_auth::{NativeCapability, NativeTransportIdentity};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_authority_wire::{
    context::NativeRenewalContext,
    live::{NativeLiveAuthorityResponse, NativeLiveFence},
    renewal::NativeRenewalFence,
    renewal_response::NativeRenewalAuthorityResponse,
    source_fence::{SourceNativeLiveFenceV2, SourceNativeRenewalFenceV3},
    source_response::{
        SourceNativeLiveAuthorityResponseV2, SourceNativeRenewalAuthorityResponseV2,
    },
};
use gateway_source::{js_string::JsString16, source_json::SourceJson, source_serialization};
use serde_json::json;
use std::collections::BTreeSet;

type TestResult = Result<(), Box<dyn std::error::Error + Send + Sync>>;
fn fence(principal: JsString16) -> SourceNativeRenewalFenceV3 {
    SourceNativeRenewalFenceV3 {
        v: 3,
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        owner_revision: "owner-r1".into(),
        native_owner_id: "native".into(),
        source_revision: "source-r1".into(),
        normalized_revision: "normalized-r1".into(),
        account_id_hash: "account".into(),
        credential_version: "credential".into(),
        source_native_generation: "native-generation".into(),
        access_generation: "access-generation".into(),
        access_expires_at_ms: None,
        native_refreshed_at: None,
        principal,
        principal_policy_digest: "digest".into(),
        original_request_id: "request".into(),
        context: NativeRenewalContext::LiveMapped {
            call_id: "call".into(),
            mapping_revision: "mapping-r1".into(),
        },
    }
}
fn live_fence(principal: JsString16) -> SourceNativeLiveFenceV2 {
    SourceNativeLiveFenceV2 {
        v: 2,
        call_id: "call".into(),
        account_id: "account".into(),
        account_id_hash: "hash".into(),
        credential_version: "credential".into(),
        source_revision: "source-r1".into(),
        normalized_revision: "normalized-r1".into(),
        mapping_revision: "mapping-r1".into(),
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        owner_revision: "owner-r1".into(),
        principal,
        principal_policy_digest: "digest".into(),
        access_expires_at_ms: None,
    }
}
#[test]
fn source_fence_and_recheck_preserve_every_original_principal_unit() -> TestResult {
    for units in [
        vec![0xd800],
        vec![0xdc00],
        vec![0xfffd],
        vec![0],
        vec![],
        vec![u16::from(b'x'); 500],
        vec![u16::from(b'x'); 501],
    ] {
        let principal = JsString16::from_units(units);
        let expected = fence(principal.clone());
        let raw = SourceJson::from_serializable(&expected)?;
        assert_eq!(
            source_serialization::member(&raw, "principal")
                .ok_or("principal missing")?
                .deserialize::<JsString16>()?,
            principal
        );
        assert_eq!(raw.deserialize::<SourceNativeRenewalFenceV3>()?, expected);
        assert!(raw.deserialize::<NativeRenewalFence>().is_err());
        for body in [
            format!(
                r#"{{"operation":"recheck","v":2,"fence":{}}}"#,
                raw.as_str()
            ),
            format!(
                r#"{{"fence":{},"v":2,"operation":"recheck"}}"#,
                raw.as_str()
            ),
        ] {
            let response: SourceNativeRenewalAuthorityResponseV2 = serde_json::from_str(&body)?;
            let SourceNativeRenewalAuthorityResponseV2::Recheck { v: 2, fence } = response else {
                return Err("wrong source recheck".into());
            };
            assert_eq!(fence, expected);
            assert!(serde_json::from_str::<NativeRenewalAuthorityResponse>(&body).is_err());
        }
    }
    Ok(())
}
#[test]
fn source_responses_refuse_mixed_missing_duplicate_and_cross_variant_fields() -> TestResult {
    let encoded = serde_json::to_string(&SourceNativeRenewalAuthorityResponseV2::Recheck {
        v: 2,
        fence: fence(JsString16::from_units(vec![0xd800])),
    })?;
    for invalid in [
        encoded.replacen("\"v\":2", "\"v\":1", 1),
        encoded.replace("\"v\":3", "\"v\":2"),
        encoded.replace("\"access_expires_at_ms\":null,", ""),
        encoded.replace("\"native_refreshed_at\":null,", ""),
        encoded.replace("\"v\":2", "\"v\":2,\"v\":2"),
        encoded.replace("\"fence\":", "\"mode\":\"renew_once\",\"fence\":"),
    ] {
        assert!(
            serde_json::from_str::<SourceNativeRenewalAuthorityResponseV2>(&invalid).is_err(),
            "{invalid}"
        );
    }
    for invalid in [
        r#"{"operation":"unknown","v":1}"#,
        r#"{"operation":"start","v":2}"#,
        r#"{"operation":"unknown","v":2,"extra":null}"#,
    ] {
        assert!(serde_json::from_str::<SourceNativeRenewalAuthorityResponseV2>(invalid).is_err());
    }
    Ok(())
}
#[test]
fn source_live_fence_streams_units_and_legacy_live_bytes_remain_exact() -> TestResult {
    let expected = live_fence(JsString16::from_units(vec![0xd800, 0, 0xdc00]));
    let response = SourceNativeLiveAuthorityResponseV2::Authorize {
        v: 2,
        authorization_ref: "authorization".into(),
        fence: expected.clone(),
        access: json!({"access_token":"SYNTHETIC_ACCESS"}),
    };
    let encoded = serde_json::to_string(&response)?;
    let decoded: SourceNativeLiveAuthorityResponseV2<serde_json::Value> =
        serde_json::from_str(&encoded)?;
    let SourceNativeLiveAuthorityResponseV2::Authorize { v: 2, fence, .. } = decoded else {
        return Err("wrong source live response".into());
    };
    assert_eq!(fence, expected);
    for invalid in [
        encoded.replacen("\"v\":2", "\"v\":1", 1),
        encoded.replacen("\"v\":2", "\"v\":2,\"v\":2", 1),
        encoded.replace(
            "\"authorization_ref\":",
            "\"outcome\":\"unknown\",\"authorization_ref\":",
        ),
    ] {
        assert!(
            serde_json::from_str::<SourceNativeLiveAuthorityResponseV2<serde_json::Value>>(
                &invalid
            )
            .is_err()
        );
    }
    let legacy = r#"{"operation":"retire","v":1,"outcome":"unknown"}"#;
    let value: NativeLiveAuthorityResponse<serde_json::Value> = serde_json::from_str(legacy)?;
    assert_eq!(serde_json::to_string(&value)?, legacy);
    let legacy_fence = serde_json::to_string(&live_fence(JsString16::from_utf8("principal")))?
        .replacen("\"v\":2,", "", 1);
    assert_eq!(
        serde_json::to_string(&serde_json::from_str::<NativeLiveFence>(&legacy_fence)?)?,
        legacy_fence
    );
    Ok(())
}
#[test]
fn unchanged_raw_body_mac_binds_new_version_principal_status_and_nonce() -> TestResult {
    let identity = NativeTransportIdentity::new(
        "fixture".into(),
        "owner".into(),
        "epoch".into(),
        [19; 32],
        BTreeSet::from([NativeCapability::OwnerRenewal]),
    )?;
    let nonce = base64url(&[23; 32]);
    let request =
        br#"{"operation":"recheck","v":1,"operation_id":"operation","claim_ref":"claim"}"#;
    let body = serde_json::to_vec(&SourceNativeRenewalAuthorityResponseV2::Recheck {
        v: 2,
        fence: fence(JsString16::from_units(vec![0xd800])),
    })?;
    let mac = identity.sign_authority_response(&nonce, request, 200, &body)?;
    identity.verify_authority_response(&nonce, request, 200, &body, &mac)?;
    for changed in [
        String::from_utf8(body.clone())?
            .replace("\\ud800", "\\udc00")
            .into_bytes(),
        String::from_utf8(body.clone())?
            .replacen("\"v\":2", "\"v\":1", 1)
            .into_bytes(),
    ] {
        assert_ne!(changed, body);
        assert!(
            identity
                .verify_authority_response(&nonce, request, 200, &changed, &mac)
                .is_err()
        );
    }
    assert!(
        identity
            .verify_authority_response(&nonce, request, 503, &body, &mac)
            .is_err()
    );
    assert!(
        identity
            .verify_authority_response(&base64url(&[24; 32]), request, 200, &body, &mac)
            .is_err()
    );
    Ok(())
}
