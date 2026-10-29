use gateway_auth_native_contracts::{
    native_live::NATIVE_LIVE_AUTHORITY_MAX_BYTES, transport::NativeCapability,
};
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_primitives::crypto::base64url;
use gateway_native_authority_wire::{
    live::NativeLiveRetirement, source_access::NativeLiveAccessWire,
    source_fence::SourceNativeLiveFenceV2,
    source_response::SourceNativeLiveAuthorityResponseV2 as Response,
};
use gateway_source::js_string::JsString16;
use std::collections::BTreeSet;

type TestResult = Result<(), Box<dyn std::error::Error + Send + Sync>>;

fn fence(principal: JsString16) -> SourceNativeLiveFenceV2 {
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

fn authorize_body(token: &str, principal: JsString16) -> Result<Vec<u8>, serde_json::Error> {
    let fence = serde_json::to_string(&fence(principal))?;
    let token = serde_json::to_string(token)?;
    Ok(format!(
        r#"{{"operation":"authorize","v":2,"authorization_ref":"authorization","fence":{fence},"access":{{"access_token":{token}}}}}"#
    )
    .into_bytes())
}

#[test]
fn finite_access_wire_preserves_scalar_strings_and_refuses_malformed_fields() -> TestResult {
    for token in ["SYNTHETIC_ACCESS", "", " ", "\0", "\u{fffd}"] {
        let encoded = format!(r#"{{"access_token":{}}}"#, serde_json::to_string(token)?);
        let decoded: NativeLiveAccessWire = serde_json::from_str(&encoded)?;
        assert_eq!(decoded.expose(), token);
    }
    for encoded in [
        r#"{}"#,
        r#"null"#,
        r#"{"access_token":null}"#,
        r#"{"access_token":4}"#,
        r#"{"access_token":{}}"#,
        r#"{"access_token":[]}"#,
        r#"{"access_token":"SYNTHETIC_ACCESS","extra":null}"#,
        r#"{"access_token":"SYNTHETIC_ACCESS","access_token":"SYNTHETIC_ACCESS"}"#,
        r#"{"accessToken":"SYNTHETIC_ACCESS"}"#,
        r#"{"access_token":"\ud800"}"#,
    ] {
        assert!(serde_json::from_str::<NativeLiveAccessWire>(encoded).is_err());
    }
    Ok(())
}

#[test]
fn typed_source_live_responses_preserve_principal_units_and_refuse_mixed_shapes() -> TestResult {
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
        let body = authorize_body("SYNTHETIC_ACCESS", principal.clone())?;
        let Response::Authorize {
            v: 2,
            fence,
            access,
            ..
        } = serde_json::from_slice::<Response<NativeLiveAccessWire>>(&body)?
        else {
            return Err("wrong source authorize shape".into());
        };
        assert_eq!(fence.principal, principal);
        assert_eq!(fence.v, 2);
        assert_eq!(access.expose(), "SYNTHETIC_ACCESS");
        let recheck = String::from_utf8(body)?
            .replace("\"operation\":\"authorize\"", "\"operation\":\"recheck\"")
            .replace("\"authorization_ref\":\"authorization\",", "");
        let Response::Recheck {
            v: 2,
            fence,
            access,
        } = serde_json::from_str::<Response<NativeLiveAccessWire>>(&recheck)?
        else {
            return Err("wrong source recheck shape".into());
        };
        assert_eq!(fence.principal, principal);
        assert_eq!(access.expose(), "SYNTHETIC_ACCESS");
    }
    let body = String::from_utf8(authorize_body(
        "SYNTHETIC_ACCESS",
        JsString16::from_utf8("principal"),
    )?)?;
    for invalid in [
        body.replacen("\"v\":2", "\"v\":1", 1),
        body.replacen("\"fence\":{\"v\":2", "\"fence\":{\"v\":3", 1),
        body.replacen("\"v\":2", "\"v\":2,\"v\":2", 1),
        body.replacen('{', "{\"outcome\":\"unknown\",", 1),
        body.replacen('{', "{\"extra\":null,", 1),
        body.replace("{\"access_token\":\"SYNTHETIC_ACCESS\"}", "null"),
        body.replace("{\"access_token\":\"SYNTHETIC_ACCESS\"}", "{}"),
        body.replace("\"access_token\":", "\"extra\":null,\"access_token\":"),
        body.replace(
            "\"access_token\":\"SYNTHETIC_ACCESS\"",
            "\"access_token\":\"SYNTHETIC_ACCESS\",\"access_token\":\"SYNTHETIC_ACCESS\"",
        ),
    ] {
        assert!(serde_json::from_str::<Response<NativeLiveAccessWire>>(&invalid).is_err());
    }
    let retired: Response<NativeLiveAccessWire> =
        serde_json::from_str(r#"{"operation":"retire","v":2,"outcome":"retired"}"#)?;
    assert!(matches!(
        retired,
        Response::Retire {
            v: 2,
            outcome: NativeLiveRetirement::Retired,
        }
    ));
    assert!(
        serde_json::from_str::<Response<NativeLiveAccessWire>>(
            r#"{"operation":"retire","v":2,"outcome":"retired","access":null}"#
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn real_live_mac_cap_and_tamper_checks_are_distinct_from_wire_data_decode() -> TestResult {
    let identity = NativeTransportIdentity::new(
        "fixture".into(),
        "owner".into(),
        "epoch".into(),
        [19; 32],
        BTreeSet::from([NativeCapability::OwnerRenewal]),
    )?;
    let nonce = base64url(&[23; 32]);
    let request =
        br#"{"operation":"authorize","v":1,"action_ref":"action","connection_id":"connection"}"#;
    let principal = JsString16::from_units(vec![0xd800, 0, 0xdc00]);
    let overhead = authorize_body("", principal.clone())?.len();
    let token = "x".repeat(NATIVE_LIVE_AUTHORITY_MAX_BYTES - overhead);
    let body = authorize_body(&token, principal)?;
    assert_eq!(body.len(), NATIVE_LIVE_AUTHORITY_MAX_BYTES);
    let mac = identity.sign_live_authority_response(&nonce, request, 200, &body)?;
    identity.verify_live_authority_response(&nonce, request, 200, &body, &mac)?;
    let Response::Authorize { v: 2, access, .. } =
        serde_json::from_slice::<Response<NativeLiveAccessWire>>(&body)?
    else {
        return Err("wrong bounded source live shape".into());
    };
    assert_eq!(access.expose().len(), token.len());
    let mut over = body.clone();
    over.push(b' ');
    assert_eq!(over.len(), NATIVE_LIVE_AUTHORITY_MAX_BYTES + 1);
    assert!(
        identity
            .sign_live_authority_response(&nonce, request, 200, &over)
            .is_err()
    );
    assert!(
        identity
            .verify_live_authority_response(&nonce, request, 200, &over, &mac)
            .is_err()
    );
    // Pure DATA decoding does not confer the failed raw-body MAC authority.
    assert!(serde_json::from_slice::<Response<NativeLiveAccessWire>>(&over).is_ok());
    let mut altered = body.clone();
    let byte = altered
        .iter_mut()
        .rfind(|byte| **byte == b'x')
        .ok_or("token byte")?;
    *byte = b'y';
    assert!(
        identity
            .verify_live_authority_response(&nonce, request, 200, &altered, &mac)
            .is_err()
    );
    assert!(
        identity
            .verify_live_authority_response(&nonce, request, 503, &body, &mac)
            .is_err()
    );
    assert!(
        identity
            .verify_live_authority_response(&base64url(&[24; 32]), request, 200, &body, &mac)
            .is_err()
    );
    assert!(
        identity
            .verify_live_authority_response(&nonce, b"{}", 200, &body, &mac)
            .is_err()
    );
    Ok(())
}
