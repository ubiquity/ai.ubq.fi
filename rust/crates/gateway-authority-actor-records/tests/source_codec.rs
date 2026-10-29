use gateway_authority_actor_records::{context, source::StoredNativeActorV2, versions};
use gateway_contracts::{identity::PrincipalId, identity::RequestContext, identity::RequestId};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};

type TestResult = Result<(), Box<dyn std::error::Error>>;

const SOURCE_ACTOR: &str = r#"{
    "v":2,
    "principal":"passport-user:\ud800\u0000",
    "scope":"admin",
    "config_digest":"config",
    "owner_revision":null,
    "config_revision":null,
    "runtime_revision":null,
    "source_valid_until_ms":null,
    "source":{
        "version":2,
        "kind":"passport_jwt",
        "data":{"sub":"\ud800\u0000","jti":"\udfff","expires_at_ms":1000}
    }
}"#;

fn source_actor() -> Result<SourceJson, GatewayError> {
    SourceJson::parse(SOURCE_ACTOR)
}

#[test]
fn actor_source_units_survive_explicit_codec_without_widening_scalar_serde() -> TestResult {
    let actor = StoredNativeActorV2::from_source_json(&source_actor()?)?;
    let suffix = actor.principal.as_source().as_units();
    assert_eq!(
        suffix.iter().copied().rev().take(2).collect::<Vec<_>>(),
        [0, 0xd800]
    );
    assert!(serde_json::to_string(&actor.principal).is_err());
    let encoded = actor.to_source_json()?;
    assert!(encoded.as_str().contains(r"\ud800\u0000"));
    assert!(encoded.as_str().contains(r"\udfff"));
    assert_eq!(StoredNativeActorV2::from_source_json(&encoded)?, actor);
    assert!(
        actor
            .framed_bytes()?
            .starts_with(versions::ACTOR_POLICY_V2_DOMAIN)
    );
    assert_eq!(versions::ACTOR_POLICY_V2_DOMAIN.last(), Some(&0));
    Ok(())
}

#[test]
fn source_actor_requires_its_version_binding_and_explicit_nullable_resource_expiry() -> TestResult {
    let original = source_actor()?;
    for text in [
        SOURCE_ACTOR.replace("\"v\":2", "\"v\":1"),
        SOURCE_ACTOR.replace("passport-user:", "another-user:"),
        SOURCE_ACTOR.replace("    \"source_valid_until_ms\":null,\n", ""),
        SOURCE_ACTOR.replace("\"kind\":\"passport_jwt\"", "\"kind\":\"unknown\""),
    ] {
        assert!(StoredNativeActorV2::from_source_json(&SourceJson::parse(&text)?).is_err());
    }
    assert_eq!(
        StoredNativeActorV2::from_source_json(&original)?.source_expiry(),
        Some(1000)
    );
    Ok(())
}

#[test]
fn source_context_preserves_principal_while_historical_context_serde_stays_scalar() -> TestResult {
    let original = RequestContext {
        request_id: RequestId("request".into()),
        principal: PrincipalId::from_source(JsString16::from_units(vec![0xdfff, 0, 0xfffd])),
        key_id: None,
        route: "/v1/responses".into(),
        started_at_ms: 7,
    };
    assert!(serde_json::to_string(&original).is_err());
    let encoded = context::to_source_json(&original)?;
    assert!(encoded.as_str().contains(r"\udfff\u0000"));
    assert_eq!(context::from_source_json(&encoded)?, original);
    Ok(())
}

#[test]
fn new_actor_cannot_downgrade_its_row_fence_or_reply_and_generation_identity_stays_shared() {
    assert!(versions::validate_renewal_pair(2, 2, 2, 1).is_err());
    assert!(versions::validate_import_pair(2, 3).is_err());
    assert!(versions::validate_live_pair(2, 1, versions::LiveFenceForm::LegacyScalar, 1).is_err());
    assert_eq!(
        versions::validate_renewal_pair(2, 3, 3, 2).ok(),
        Some(versions::ActorForm::SourceV2)
    );
    assert_eq!(
        versions::GENERATION_V1_DOMAIN,
        "uos-native-renewal-generation-v1"
    );
}
