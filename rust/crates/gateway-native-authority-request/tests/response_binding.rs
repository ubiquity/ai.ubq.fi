use gateway_auth_native_wire::control::{
    NATIVE_RENEWAL_MAX_MS, NativeControlEnvelope, NativeControlRequest,
};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_authority_request::{
    NativeRenewalAuthorityRequest as Request, OriginalControlHeaders,
    response::{RenewalResponseVersion, identifier, response_version, validate, validate_source},
};
use gateway_native_authority_wire::{
    context::NativeRenewalContext,
    renewal::{NativeRenewalFence, NativeRenewalMode as Mode},
    renewal_response::NativeRenewalAuthorityResponse as Legacy,
    source_fence::SourceNativeRenewalFenceV3,
    source_response::SourceNativeRenewalAuthorityResponseV2 as Source,
};
use gateway_source::js_string::JsString16;

type TestResult = Result<(), Box<dyn std::error::Error + Send + Sync>>;
const NOW: i64 = 1_000;
const PEER: (&str, &str) = ("owner", "epoch");

fn request() -> Result<Request, serde_json::Error> {
    let body = serde_json::to_vec(&NativeControlEnvelope {
        v: 1,
        operation_id: "operation".into(),
        request: NativeControlRequest::CodexRenewAccess {
            action_ref: "action".into(),
        },
    })?;
    Ok(Request::Claim {
        v: 1,
        original_control_body_base64url: base64url(&body),
        original_auth_headers: OriginalControlHeaders {
            version: "1".into(),
            key_id: "synthetic".into(),
            time_ms: NOW.to_string(),
            nonce: "synthetic".into(),
            mac: "synthetic".into(),
        },
    })
}

fn fence() -> NativeRenewalFence {
    NativeRenewalFence {
        v: 2,
        owner_id: PEER.0.into(),
        owner_epoch: PEER.1.into(),
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
        principal: "principal".into(),
        principal_policy_digest: "policy".into(),
        original_request_id: "request".into(),
        context: NativeRenewalContext::LiveMapped {
            call_id: "call".into(),
            mapping_revision: "mapping-r1".into(),
        },
    }
}

fn source_fence(units: Vec<u16>) -> SourceNativeRenewalFenceV3 {
    SourceNativeRenewalFenceV3 {
        v: 3,
        owner_id: PEER.0.into(),
        owner_epoch: PEER.1.into(),
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
        principal_policy_digest: "policy".into(),
        original_request_id: "request".into(),
        context: NativeRenewalContext::LiveMapped {
            call_id: "call".into(),
            mapping_revision: "mapping-r1".into(),
        },
    }
}

fn legacy_claim() -> Legacy {
    Legacy::Claim {
        v: 1,
        action_ref: "action".into(),
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        start_permit: Some("permit".into()),
        mode: Mode::RenewOnce,
        deadline_ms: NOW + NATIVE_RENEWAL_MAX_MS,
        fence: fence(),
        publication_revision: None,
    }
}

fn source_claim(units: Vec<u16>) -> Source {
    Source::Claim {
        v: 2,
        action_ref: "action".into(),
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        start_permit: Some("permit".into()),
        mode: Mode::RenewOnce,
        deadline_ms: NOW + NATIVE_RENEWAL_MAX_MS,
        fence: source_fence(units),
        publication_revision: None,
    }
}

#[test]
fn claim_modes_deadline_boundaries_and_source_units_keep_the_original_domain() -> TestResult {
    let request = request()?;
    validate(&request, &legacy_claim(), NOW, PEER)?;
    for units in [
        vec![0xd800],
        vec![0xdc00],
        vec![0xfffd],
        vec![0],
        vec![],
        vec![120; 501],
    ] {
        validate_source(&request, &source_claim(units), NOW, PEER)?;
    }
    let mut legacy = legacy_claim();
    let mut source = source_claim(vec![0xd800, 0, 0xdc00]);
    if let Legacy::Claim {
        mode,
        start_permit,
        deadline_ms,
        publication_revision,
        ..
    } = &mut legacy
    {
        *mode = Mode::ReconcileOnly;
        *start_permit = None;
        *deadline_ms = 0;
        *publication_revision = Some("publication".into());
    }
    if let Source::Claim {
        mode,
        start_permit,
        deadline_ms,
        publication_revision,
        ..
    } = &mut source
    {
        *mode = Mode::ReconcileOnly;
        *start_permit = None;
        *deadline_ms = 0;
        *publication_revision = Some("publication".into());
    }
    // Shape/currentness DATA is not a new Start or an active request lifetime.
    validate(&request, &legacy, NOW, PEER)?;
    validate_source(&request, &source, NOW, PEER)?;
    Ok(())
}

fn corrupt_legacy(response: &mut Legacy, scenario: u8) {
    let Legacy::Claim {
        v,
        action_ref,
        operation_id,
        claim_ref,
        start_permit,
        mode,
        deadline_ms,
        fence,
        publication_revision,
    } = response
    else {
        unreachable!()
    };
    match scenario {
        0 => *v = 2,
        1 => fence.v = 3,
        2 => *action_ref = "other".into(),
        3 => *operation_id = "other".into(),
        4 => claim_ref.clear(),
        5 => fence.owner_id = "other".into(),
        6 => fence.owner_epoch = "other".into(),
        7 => *start_permit = None,
        8 => *deadline_ms = NOW,
        9 => *deadline_ms += 1,
        10 => *mode = Mode::ReconcileOnly,
        11 => *publication_revision = Some("publication".into()),
        _ => {
            *mode = Mode::ReconcileOnly;
            *start_permit = None;
            *publication_revision = Some(String::new());
        }
    }
}

fn corrupt_source(response: &mut Source, scenario: u8) {
    let Source::Claim {
        v,
        action_ref,
        operation_id,
        claim_ref,
        start_permit,
        mode,
        deadline_ms,
        fence,
        publication_revision,
    } = response
    else {
        unreachable!()
    };
    match scenario {
        0 => *v = 1,
        1 => fence.v = 2,
        2 => *action_ref = "other".into(),
        3 => *operation_id = "other".into(),
        4 => claim_ref.clear(),
        5 => fence.owner_id = "other".into(),
        6 => fence.owner_epoch = "other".into(),
        7 => *start_permit = None,
        8 => *deadline_ms = NOW,
        9 => *deadline_ms += 1,
        10 => *mode = Mode::ReconcileOnly,
        11 => *publication_revision = Some("publication".into()),
        _ => {
            *mode = Mode::ReconcileOnly;
            *start_permit = None;
            *publication_revision = Some(String::new());
        }
    }
}

#[test]
fn claim_correlation_refuses_each_wrong_peer_field_mode_and_version() -> TestResult {
    let request = request()?;
    for scenario in 0..13 {
        let mut legacy = legacy_claim();
        let mut source = source_claim(vec![0xd800]);
        corrupt_legacy(&mut legacy, scenario);
        corrupt_source(&mut source, scenario);
        assert!(validate(&request, &legacy, NOW, PEER).is_err());
        assert!(validate_source(&request, &source, NOW, PEER).is_err());
    }
    Ok(())
}

#[test]
fn every_non_claim_operation_keeps_its_exact_pair_peer_and_publication() -> TestResult {
    let recheck = Request::Recheck {
        v: 1,
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
    };
    validate(
        &recheck,
        &Legacy::Recheck {
            v: 1,
            fence: fence(),
        },
        NOW,
        PEER,
    )?;
    validate_source(
        &recheck,
        &Source::Recheck {
            v: 2,
            fence: source_fence(vec![0xd800]),
        },
        NOW,
        PEER,
    )?;
    assert!(
        validate(
            &recheck,
            &Legacy::Recheck {
                v: 1,
                fence: fence()
            },
            NOW,
            ("other", PEER.1)
        )
        .is_err()
    );
    assert!(
        validate_source(
            &recheck,
            &Source::Recheck {
                v: 2,
                fence: source_fence(vec![0xd800])
            },
            NOW,
            (PEER.0, "other")
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn start_and_unknown_keep_their_exact_pair_without_issuing_effects() -> TestResult {
    let start = Request::Start {
        v: 1,
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        start_permit: "permit".into(),
    };
    // The complete physical guard, not this DATA shape, requires RenewOnce.
    validate(
        &start,
        &Legacy::Start {
            v: 1,
            mode: Mode::ReconcileOnly,
        },
        NOW,
        PEER,
    )?;
    validate_source(
        &start,
        &Source::Start {
            v: 2,
            mode: Mode::ReconcileOnly,
        },
        NOW,
        PEER,
    )?;
    let unknown = Request::Unknown {
        v: 1,
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
    };
    validate(&unknown, &Legacy::Unknown { v: 1 }, NOW, PEER)?;
    validate_source(&unknown, &Source::Unknown { v: 2 }, NOW, PEER)?;
    assert!(validate(&start, &Legacy::Unknown { v: 1 }, NOW, PEER).is_err());
    assert!(
        validate_source(
            &unknown,
            &Source::Start {
                v: 2,
                mode: Mode::RenewOnce
            },
            NOW,
            PEER
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn published_response_requires_the_original_revision_and_version() -> TestResult {
    let published = Request::Published {
        v: 1,
        operation_id: "operation".into(),
        claim_ref: "claim".into(),
        publication_revision: "publication".into(),
    };
    validate(
        &published,
        &Legacy::Published {
            v: 1,
            publication_revision: "publication".into(),
        },
        NOW,
        PEER,
    )?;
    validate_source(
        &published,
        &Source::Published {
            v: 2,
            publication_revision: "publication".into(),
        },
        NOW,
        PEER,
    )?;
    assert!(
        validate(
            &published,
            &Legacy::Published {
                v: 1,
                publication_revision: "other".into()
            },
            NOW,
            PEER
        )
        .is_err()
    );
    assert!(
        validate_source(
            &published,
            &Source::Published {
                v: 1,
                publication_revision: "publication".into()
            },
            NOW,
            PEER
        )
        .is_err()
    );
    Ok(())
}

#[test]
fn raw_version_is_duplicate_checked_data_and_never_a_typed_response_grant() -> TestResult {
    assert!(response_version(br#"{"v":1}"#)? == RenewalResponseVersion::Legacy);
    let source = br#"{"v":2,"principal":"\ud800"}"#;
    assert!(response_version(source)? == RenewalResponseVersion::Source);
    assert!(serde_json::from_slice::<Source>(source).is_err());
    for invalid in [
        br#"{}"#.as_slice(),
        br#"[]"#,
        br#"{"v":0}"#,
        br#"{"v":3}"#,
        br#"{"v":256}"#,
        br#"{"v":-1}"#,
        br#"{"v":null}"#,
        br#"{"v":"2"}"#,
        br#"{"v":1,"v":1}"#,
        br#"{"v":1,"\u0076":2}"#,
    ] {
        assert!(response_version(invalid).is_err());
    }
    Ok(())
}

#[test]
fn reference_domain_keeps_the_exact_original_ascii_and_byte_boundaries() {
    for valid in ["a", "Z09-_.:", &"a".repeat(256)] {
        assert!(identifier(valid));
    }
    for invalid in ["", " ", "a/b", "\0", "é", &"a".repeat(257)] {
        assert!(!identifier(invalid));
    }
}

#[test]
fn original_control_decode_errors_keep_their_original_wire_envelope() -> TestResult {
    let mut request = request()?;
    if let Request::Claim {
        original_control_body_base64url,
        ..
    } = &mut request
    {
        *original_control_body_base64url = "***".into();
    }
    let expected = request
        .original_control_body()
        .err()
        .ok_or("invalid body accepted")?;
    let legacy = validate(&request, &legacy_claim(), NOW, PEER)
        .err()
        .ok_or("invalid legacy body accepted")?;
    let source = validate_source(&request, &source_claim(vec![0xd800]), NOW, PEER)
        .err()
        .ok_or("invalid source body accepted")?;
    for actual in [legacy, source] {
        assert_eq!(actual.status, expected.status);
        assert_eq!(actual.envelope.error.code, expected.envelope.error.code);
        assert_eq!(
            actual.envelope.error.message,
            expected.envelope.error.message
        );
    }
    Ok(())
}
