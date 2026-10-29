mod callback_inference_fixture;
mod inference_fixture;
use callback_inference_fixture::{
    Cancel, Host, TestResult, control_body, execute, inference_input, peer, refusal, run,
};
use gateway_authority_actor_currentness::app::ActorService;
use gateway_authority_native_preparation::{
    SourceNativePreparationContext, SourceNativeRenewalService,
};
use gateway_authority_native_records::{owner::RegisteredRenewalOwner, rows};
use gateway_core::{source_control::decode_source_control_record, state::AuthorityStore};
use gateway_kv_codec::value::ValueEncoding;
use gateway_local_journal::context::decode_journal_context;
use gateway_routing_runtime::session::RoutingSession;
use gateway_routing_runtime_test_support::{
    input::NOW as RUNTIME_NOW, store::Store as RuntimeStore,
};
use gateway_source::source_serialization as source;
use inference_fixture::NativeRefreshHost;
use std::{
    future::Future,
    task::{Context, Waker},
};

#[test]
fn source_callback_inference_pairs_response2_and_original_utf16_context() -> TestResult {
    run(async {
        let store = RuntimeStore::new();
        let auth = Host::default();
        let (input, request, native, config) =
            inference_input(&store, &auth, "native-source-inference").await?;
        let original = input.context.clone();
        assert!(original.principal.try_scalar().is_err());
        let source_actor = ActorService::new(&store, &auth, &config)
            .authenticate_source_client_actor(&request, &original)
            .await?;
        let (host, pause) = NativeRefreshHost::new(&store);
        let session =
            RoutingSession::begin(input, store.clone(), host, store.clone(), store.clone())
                .await
                .map_err(|error| *error.error)?;
        let paid_request = gateway_authority_paid_records::PaidRequest {
            key_id: gateway_contracts::identity::ApiKeyId("paid-target".into()),
            request_id: original.request_id.clone(),
            attempt_id: gateway_contracts::identity::PaidAttemptId("paid-attempt".into()),
            provider: gateway_contracts::provider::PaidProvider::Surplus,
            model: gateway_contracts::model::ModelId::from("gpt-test"),
            route: original.route.clone(),
            path: "/v1/responses".into(),
            stream: false,
            reasoning: None,
            created_at_ms: original.started_at_ms,
        };
        let admitted_record = store
            .read(&rows::session_key(&original.request_id.0))
            .await?
            .ok_or("admitted journal")?;
        let paid_checks = gateway_authority_paid_evidence::capture_original(
            &store,
            &paid_request,
            &original,
            None,
            RUNTIME_NOW,
        )
        .await?;
        assert!(paid_checks.iter().any(|check| {
            check.key == rows::session_key(&original.request_id.0)
                && check.expected_revision.as_deref() == Some(admitted_record.revision.as_str())
        }));
        let attempt = session
            .next_attempt()
            .await
            .map_err(|error| *error.error)?
            .ok_or("attempt")?;
        session
            .before_dispatch(&attempt)
            .await
            .map_err(|error| *error.error)?;
        session
            .transport_started(&attempt)
            .await
            .map_err(|error| *error.error)?;
        let mut refresh = Box::pin(session.refusal(&attempt, refusal()));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(refresh.as_mut().poll(&mut cx).is_pending());
        let raw = store
            .read(&rows::session_key(&original.request_id.0))
            .await?
            .ok_or("journal")?;
        assert_eq!(raw.value_encoding, ValueEncoding::ProfileV2);
        gateway_routing_runtime::custody::validate_journal_row(
            &rows::session_key(&original.request_id.0),
            &raw,
        )?;
        let paid_error = gateway_authority_paid_evidence::capture_original(
            &store,
            &paid_request,
            &original,
            None,
            RUNTIME_NOW,
        )
        .await
        .err()
        .ok_or("active attempt accepted by prospective paid capture")?;
        assert_eq!(paid_error.status, 503);
        assert_eq!(
            paid_error.envelope.error.code.as_deref(),
            Some("paid_control_unavailable")
        );
        assert_eq!(
            paid_error.envelope.error.message,
            "Original request ownership is missing, expired, finalizing or changed."
        );
        let journal = decode_source_control_record(&raw)?;
        assert_eq!(decode_journal_context(&journal)?, original);
        assert_eq!(source::member(&journal, "v").ok_or("v")?.as_str(), "3");
        let refreshed = source::member(&journal, "refreshed_versions").ok_or("refreshed")?;
        assert!(
            !source::array(&refreshed)
                .ok_or("refreshed array")?
                .is_empty()
        );
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let service = SourceNativeRenewalService::new(
            &store,
            &store,
            Some(&store),
            &auth,
            &config,
            SourceNativePreparationContext::configured(&native, &owner, &cancel),
        );
        let action = service
            .prepare_source_inference_renewal(&source_actor)
            .await?;
        assert_eq!(action.fence().v, 3);
        assert_eq!(action.fence().principal, *original.principal.as_source());
        let gateway_native_authority_wire::context::NativeRenewalContext::Inference {
            attempt_id,
            model,
            ..
        } = &action.fence().context
        else {
            return Err("wrong inference context".into());
        };
        assert_eq!(attempt_id, &attempt.id);
        assert_eq!(model, &gateway_contracts::model::ModelId::from("gpt-test"));
        let row = store
            .read(&rows::key("action", action.action_ref()))
            .await?
            .ok_or("action")?;
        let row=gateway_authority_native_records::source_rows::SourceRenewalActionRowV3::from_source_json(&decode_source_control_record(&row)?)?;
        assert_eq!(row.context, original);
        let lifecycle = gateway_authority_native_lifecycle::SourceNativeLifecycleService::new(
            &store,
            &store,
            Some(&store),
            &auth,
            &config,
            gateway_authority_native_lifecycle::SourceNativeLifecycleContext::configured(
                &native, &owner, &cancel,
            ),
        );
        let now = gateway_authority_contracts::AuthorityClock::now_ms(&store)?;
        let original_control = control_body(action.action_ref(), "callback-inference-operation")?;
        let nonce = gateway_auth_primitives::crypto::base64url(&[180; 32]);
        let request = gateway_native_authority_request::NativeRenewalAuthorityRequest::Claim {
            v: 1,
            original_control_body_base64url: gateway_auth_primitives::crypto::base64url(
                &original_control,
            ),
            original_auth_headers: gateway_native_authority_request::OriginalControlHeaders {
                version: "1".into(),
                key_id: native.key_id().into(),
                time_ms: now.to_string(),
                nonce: nonce.clone(),
                mac: native.sign(
                    gateway_auth_native_wire::native_read::NATIVE_CONTROL_PATH,
                    now,
                    &nonce,
                    &original_control,
                )?,
            },
        };
        let raw = serde_json::to_vec(&request)?;
        let replay = gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(&store);
        let peer = peer(
            &native,
            &replay,
            gateway_auth_native_wire::native_read::NATIVE_RENEWAL_AUTHORITY_PATH,
            &raw,
            now,
            181,
        )
        .await?;
        let response = gateway_authority_native_callback::execute_source(
            &lifecycle, &peer, &native, &replay, &store, &raw,
        )
        .await?;
        let gateway_native_authority_wire::source_response::SourceNativeRenewalAuthorityResponseV2::Claim{v:2,fence,claim_ref,..}=response else{return Err("source inference callback".into())};
        assert_eq!(&fence.principal, original.principal.as_source());
        assert!(
            matches!(fence.context,gateway_native_authority_wire::context::NativeRenewalContext::Inference{ref attempt_id,..} if attempt_id==&attempt.id)
        );
        let recheck = gateway_native_authority_request::NativeRenewalAuthorityRequest::Recheck {
            v: 1,
            operation_id: "callback-inference-operation".into(),
            claim_ref,
        };
        assert!(matches!(execute(&lifecycle,&store,&auth,&native,&recheck,182).await?,gateway_native_authority_wire::source_response::SourceNativeRenewalAuthorityResponseV2::Recheck{v:2,..}));
        pause.release();
        refresh.await.map_err(|error| *error.error)?;
        assert!(
            service
                .prepare_source_inference_renewal(&source_actor)
                .await
                .is_err()
        );
        Ok(())
    })
}
