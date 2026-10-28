//! Actual signed source response DATA; no source CurrentGuard/Start is issued.
use super::{context, identity, pair};
use crate::authority_http::NativeAuthorityHttp;
use gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH;
use gateway_authority::native_control::wire::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    context::NativeRenewalContext, source_fence::SourceNativeRenewalFenceV3,
    source_response::SourceNativeRenewalAuthorityResponseV2 as Response,
};
use gateway_source::{error::GatewayError, js_string::JsString16};
type SetupResult<T> = Result<T, Box<dyn std::error::Error + Send + Sync>>;

fn fence(units: Vec<u16>) -> SourceNativeRenewalFenceV3 {
    SourceNativeRenewalFenceV3 {
        v: 3,
        owner_id: "fixture-owner".into(),
        owner_epoch: "fixture-epoch".into(),
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
        principal: JsString16::from_units(units),
        principal_policy_digest: "digest".into(),
        original_request_id: "request".into(),
        context: NativeRenewalContext::LiveMapped {
            call_id: "call".into(),
            mapping_revision: "mapping-r1".into(),
        },
    }
}
fn exchange(
    value: SourceNativeRenewalFenceV3,
    replayed: bool,
    version: u8,
) -> SetupResult<Result<Response, GatewayError>> {
    let (client, peer, port) = pair();
    let server = gateway_native_https_test_support::renewal_tls::signed_recheck(
        peer,
        identity(),
        value,
        replayed,
        version,
    )?;
    let ca = ureq::tls::Certificate::from_der(include_bytes!(
        "../../../tests/fixtures/native-tls/ca.der"
    ))
    .to_owned();
    let authority = NativeAuthorityHttp::with_roots(
        &format!("https://localhost:{port}{NATIVE_RENEWAL_AUTHORITY_PATH}"),
        client,
        identity(),
        ureq::tls::RootCerts::new_with_certs(&[ca]),
    )?;
    let result = authority.exchange_source(
        &Request::Recheck {
            v: 1,
            operation_id: "new-opaque-operation".into(),
            claim_ref: "new-opaque-claim".into(),
        },
        &context(),
    );
    server
        .join()
        .map_err(|_| "fixed source TLS peer panicked")??;
    Ok(result)
}
#[test]
fn actual_signed_source_response2_fence3_keeps_principal_units_without_minting_effects()
-> SetupResult<()> {
    for units in [
        vec![0xd800],
        vec![0xdc00],
        vec![0xfffd],
        vec![0],
        vec![],
        vec![u16::from(b'x'); 500],
        vec![u16::from(b'x'); 501],
    ] {
        let expected = fence(units);
        let response = exchange(expected.clone(), false, 2)??;
        let Response::Recheck { v: 2, fence } = response else {
            return Err("wrong source response".into());
        };
        assert_eq!(fence, expected);
    }
    Ok(())
}
#[test]
fn actual_signed_source_rechecks_refuse_wrong_owner_epoch_version_and_replayed_body()
-> SetupResult<()> {
    for scenario in 0..5 {
        let mut value = fence(vec![0xd800]);
        if scenario == 0 {
            value.owner_id = "other-owner".into();
        }
        if scenario == 1 {
            value.owner_epoch = "other-epoch".into();
        }
        if scenario == 2 {
            value.v = 2;
        }
        let error = exchange(value, scenario == 4, if scenario == 3 { 1 } else { 2 })?
            .err()
            .ok_or("invalid source response accepted")?;
        assert_eq!(error.status, 503);
        assert_eq!(
            error.envelope.error.code.as_deref(),
            Some("native_control_outcome_unknown")
        );
    }
    Ok(())
}
