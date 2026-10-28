use gateway_auth_native_contracts::native_import::{
    NativeImportKind, NativeImportMetadata, NativeImportTarget,
};
use gateway_auth_passport_contracts::actor_data::{PassportActorDataV2, PassportAppSessionBinding};
use gateway_authority_actor_records::{
    context, scope::NativeActorScope, source::StoredNativeActorV2, versions,
};
use gateway_authority_native_records::{
    import_facts::ImportFacts,
    rows,
    source_rows::{SourceImportActionRowV4, SourceLiveActionRowV2, SourceRenewalActionRowV3},
};
use gateway_contracts::{
    identity::{PrincipalId, RequestContext, RequestId},
    model::ModelId,
};
use gateway_native_authority_wire::{
    context::NativeRenewalContext, renewal::NativeRenewalFence,
    source_fence::SourceNativeRenewalFenceV3,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
type TestResult = Result<(), Box<dyn std::error::Error>>;
fn actor() -> StoredNativeActorV2 {
    let subject = JsString16::from_units(vec![0xd800, 0]);
    let principal = PrincipalId::from_source(JsString16::from_units(
        "passport-user:"
            .encode_utf16()
            .chain(subject.as_units().iter().copied())
            .collect(),
    ));
    StoredNativeActorV2 {
        v: 2,
        principal,
        scope: NativeActorScope::Client,
        config_digest: "config".into(),
        owner_revision: Some("owner".into()),
        config_revision: Some("config".into()),
        runtime_revision: Some("runtime".into()),
        source_valid_until_ms: None,
        source: PassportActorDataV2::PassportAppSession(PassportAppSessionBinding {
            session_digest: "a".repeat(64),
            row_revision: "session".into(),
            subject,
            created_at: JsString16::from_utf8("10/27/2026"),
            app_token_digest: "b".repeat(64),
            client_id: "ai".into(),
            audience: "ai".into(),
            origin: "https://ai.ubq.fi".into(),
            redirect_uri: "https://ai.ubq.fi/auth/callback".into(),
        }),
    }
}
fn original(actor: &StoredNativeActorV2) -> RequestContext {
    RequestContext {
        request_id: RequestId("request".into()),
        principal: actor.principal.clone(),
        key_id: None,
        route: "responses".into(),
        started_at_ms: 1,
    }
}
fn renewal() -> Result<SourceRenewalActionRowV3, GatewayError> {
    let actor = actor();
    let context = original(&actor);
    let fence = SourceNativeRenewalFenceV3 {
        v: 3,
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        owner_revision: "owner".into(),
        native_owner_id: "native".into(),
        source_revision: "source".into(),
        normalized_revision: "normalized".into(),
        account_id_hash: "account".into(),
        credential_version: "credential".into(),
        source_native_generation: "physical".into(),
        access_generation: "access".into(),
        access_expires_at_ms: None,
        native_refreshed_at: None,
        principal: actor.principal.as_source().clone(),
        principal_policy_digest: actor.policy_digest()?,
        original_request_id: context.request_id.0.clone(),
        context: NativeRenewalContext::Inference {
            attempt_id: "attempt".into(),
            original_admission_digest: "admission".into(),
            model: ModelId::from_source(JsString16::from_units(vec![0xdfff])),
            slot: 0,
        },
    };
    Ok(SourceRenewalActionRowV3 {
        v: 3,
        action_ref: "action".into(),
        actor,
        context,
        created_at_ms: 1,
        deadline_ms: 45_001,
        fence,
        checks_digest: "checks".into(),
        control_checks_digest: "control".into(),
        account_digests: vec!["account".into()],
        operation_id: None,
    })
}
fn facts() -> ImportFacts {
    ImportFacts {
        owner_id: "owner".into(),
        owner_epoch: "epoch".into(),
        native_owner_id: "native".into(),
        slot: 0,
        account_id_hash: "account".into(),
        credential_version: "credential".into(),
        access_generation: "access".into(),
        source_native_generation: "physical".into(),
        owner_revision: "owner".into(),
        source_revision: "source".into(),
        normalized_revision: "normalized".into(),
        runtime_revision: None,
        control_checks_digest: "control".into(),
        account_digests: vec![],
        checks_digest: "checks".into(),
    }
}
#[test]
fn renewal_source_row_preserves_exact_actor_context_and_fence_units() -> TestResult {
    let original = renewal()?;
    let encoded = original.to_source_json()?;
    assert!(encoded.as_str().contains(r"\ud800\u0000"));
    assert!(encoded.as_str().contains(r"\udfff"));
    let decoded = SourceRenewalActionRowV3::from_source_json(&encoded)?;
    assert_eq!(decoded.actor, original.actor);
    assert_eq!(decoded.context, original.context);
    assert_eq!(decoded.fence, original.fence);
    let context = context::to_source_json(&original.context)?;
    let missing = encoded
        .as_str()
        .replacen(&format!("\"context\":{},", context.as_str()), "", 1);
    assert!(SourceRenewalActionRowV3::from_source_json(&SourceJson::parse(&missing)?).is_err());
    Ok(())
}
#[test]
fn mixed_actor_fence_or_original_context_cannot_roundtrip_as_source_action() -> TestResult {
    let mut row = renewal()?;
    row.fence.v = 2;
    assert!(row.to_source_json().is_err());
    row.fence.v = 3;
    row.context.request_id = RequestId("another".into());
    assert!(row.to_source_json().is_err());
    row.context = original(&row.actor);
    row.fence.principal = JsString16::from_utf8("another");
    assert!(row.to_source_json().is_err());
    Ok(())
}
#[test]
fn import_source_row_has_required_nullable_publication_and_lossless_context() -> TestResult {
    let actor = actor();
    let context = original(&actor);
    let row = SourceImportActionRowV4 {
        v: 4,
        kind: NativeImportKind::CodexAuth,
        metadata: NativeImportMetadata {
            v: 1,
            operation_id: "operation".into(),
            action_ref: "action".into(),
        },
        target: NativeImportTarget::ExistingAccount {
            account_id_hash: "c".repeat(64),
        },
        actor,
        context,
        created_at_ms: 1,
        deadline_ms: 45_001,
        facts: facts(),
        publication: None,
    };
    let encoded = row.to_source_json()?;
    assert_eq!(
        SourceImportActionRowV4::from_source_json(&encoded)?.context,
        row.context
    );
    let missing = encoded.as_str().replace(",\"publication\":null", "");
    assert!(SourceImportActionRowV4::from_source_json(&SourceJson::parse(&missing)?).is_err());
    let legacy = encoded.as_str().replacen("\"v\":4", "\"v\":3", 1);
    assert!(SourceImportActionRowV4::from_source_json(&SourceJson::parse(&legacy)?).is_err());
    Ok(())
}
#[test]
fn live_source_row_cannot_reconstruct_missing_original_context() -> TestResult {
    let actor = actor();
    let context = original(&actor);
    let row = SourceLiveActionRowV2 {
        v: 2,
        action_ref: "action".into(),
        request_id: context.request_id.0.clone(),
        actor,
        context,
        call_id: "call".into(),
        account_id: "account".into(),
        mapping_revision: "mapping".into(),
        created_at_ms: 1,
    };
    let encoded = row.to_source_json()?;
    let decoded = SourceLiveActionRowV2::from_source_json(&encoded)?;
    assert_eq!(decoded.actor, row.actor);
    assert_eq!(decoded.context, row.context);
    let missing = encoded.as_str().replacen(
        &format!(
            "\"context\":{},",
            context::to_source_json(&row.context)?.as_str()
        ),
        "",
        1,
    );
    assert!(SourceLiveActionRowV2::from_source_json(&SourceJson::parse(&missing)?).is_err());
    Ok(())
}
#[test]
fn legacy_and_source_rows_share_the_original_physical_generation_identity() -> TestResult {
    let source = renewal()?.fence;
    let legacy = NativeRenewalFence {
        v: 2,
        owner_id: source.owner_id.clone(),
        owner_epoch: source.owner_epoch.clone(),
        owner_revision: source.owner_revision.clone(),
        native_owner_id: source.native_owner_id.clone(),
        source_revision: source.source_revision.clone(),
        normalized_revision: source.normalized_revision.clone(),
        account_id_hash: source.account_id_hash.clone(),
        credential_version: source.credential_version.clone(),
        source_native_generation: source.source_native_generation.clone(),
        access_generation: "another-access".into(),
        access_expires_at_ms: source.access_expires_at_ms,
        native_refreshed_at: source.native_refreshed_at.clone(),
        principal: "legacy".into(),
        principal_policy_digest: "legacy".into(),
        original_request_id: source.original_request_id.clone(),
        context: source.context.clone(),
    };
    let key = rows::generation_key_parts(
        &source.native_owner_id,
        &source.account_id_hash,
        &source.source_native_generation,
    )?;
    assert_eq!(rows::generation_key(&legacy)?, key);
    let digest = gateway_auth_primitives::crypto::sha256_hex(&serde_json::to_vec(&(
        versions::GENERATION_V1_DOMAIN,
        &source.native_owner_id,
        &source.account_id_hash,
        &source.source_native_generation,
    ))?);
    assert_eq!(key, rows::key("generation", &digest));
    Ok(())
}
