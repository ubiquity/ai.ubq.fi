mod callback_actor_support;
mod callback_support;
mod support;
use gateway_auth_contracts::types::AuthConfig;
use gateway_authority::store::Authority;
use gateway_authority_contracts::AuthorityClock;
use gateway_authority_native_records::{live::mapping_key, owner::RegisteredRenewalOwner, rows};
use gateway_core::{
    source_control::encode_source_control_record,
    state::{AuthorityStore, StoredRecord},
};
use gateway_native_authority_request::NativeRenewalAuthorityRequest as Request;
use gateway_native_authority_wire::{
    renewal::NativeRenewalMode, renewal_response::NativeRenewalAuthorityResponse as Legacy,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use support::{Cancel, Host, NOW, Store, TestResult, Tokens, fixture, identity, run};
struct Clock;
impl AuthorityClock for Clock {
    fn now_ms(&self) -> Result<i64, GatewayError> {
        Ok(NOW)
    }
}

#[test]
fn callback_old_and_source_forms_share_one_actual_physical_generation_start() -> TestResult {
    run(async {
        use gateway_authority_test_support::native_actor as legacy_input;
        let store = legacy_input::Store::default();
        let token = legacy_input::session_setup(&store, true, true);
        // Ordinary local-session fixture expiry is moved to this test's actual clock before authentication.
        for row in store.rows.lock().map_err(|_| "rows")?.values_mut() {
            let mut value = row.json_value()?.clone();
            if let Some(object) = value.as_object_mut() {
                for key in ["expiresAt", "expiresAtMs", "expires_at_ms"] {
                    if object.contains_key(key) {
                        object.insert(key.into(), serde_json::json!(NOW + 60_000));
                    }
                }
            }
            *row = StoredRecord::json(
                row.revision.clone(),
                value,
                row.expires_at_ms.map(|_| NOW + 60_000),
            );
        }
        let host = Host::default();
        let config = AuthConfig::default();
        let id = identity()?;
        let inputs = Store::default();
        fixture(&inputs, &id, None)?;
        for (key, row) in inputs.0.borrow().iter() {
            store
                .rows
                .lock()
                .map_err(|_| "rows")?
                .insert(key.clone(), row.clone());
        }
        let authority = Authority::with_clock(store.clone(), Tokens::default(), Clock);
        let original = legacy_input::request(&token);
        let context = legacy_input::context("passkey-user:user-fixture", None);
        let old = gateway_authority::native_control::actor::authenticate_client_actor(
            &authority, &host, &config, &original, &context,
        )
        .await?;
        let (source, _) =
            callback_actor_support::actor(authority.store(), &host, &config, false).await?;
        let map = SourceJson::parse(
            r#"{"account_id":"account","principal_id":"passkey-user:user-fixture"}"#,
        )?;
        store.rows.lock().map_err(|_| "rows")?.insert(
            mapping_key("legacy-call", false).0,
            encode_source_control_record("legacy-call", &map, None)?,
        );
        let owner =
            RegisteredRenewalOwner::configured("native".into(), "/tmp/native-source".into())?;
        let cancel = Cancel(false);
        let legacy =
            gateway_authority::native_control::receiver::NativeRenewalAuthority::configured(
                &authority, &id, &owner, &config, &cancel,
            );
        let old_action = legacy
            .prepare_live_mapped_renewal(&old, "legacy-call")
            .await?;
        let prep = authority.source_native_renewals(&host, &config, &id, &owner, &cancel);
        let action = prep
            .prepare_source_live_mapped_renewal(&source, "call")
            .await?;
        assert_eq!(
            old_action.fence().source_native_generation,
            action.fence().source_native_generation
        );
        let lifecycle = authority.source_native_lifecycle(&host, &config, &id, &owner, &cancel);
        let (claim_ref, permit) = callback_support::decoded_claim(
            callback_support::execute(
                &lifecycle,
                authority.store(),
                &host,
                &id,
                &callback_support::claim(&id, action.action_ref(), "source-operation", 230)?,
                130,
            )
            .await?,
        )?;
        let start = Request::Start {
            v: 1,
            operation_id: "source-operation".into(),
            claim_ref,
            start_permit: permit,
        };
        callback_support::execute(&lifecycle, authority.store(), &host, &id, &start, 131).await?;
        let request =
            callback_support::claim(&id, old_action.action_ref(), "legacy-operation", 232)?;
        let replay = gateway_auth_native_protocol::native_replay::StoreNonceAuthority::new(
            authority.store(),
        );
        let result = gateway_authority_native_callback::execute_legacy(
            &legacy,
            &id,
            &replay,
            &Clock,
            &serde_json::to_vec(&request)?,
        )
        .await;
        if let Ok(Legacy::Claim {
            mode, start_permit, ..
        }) = result
        {
            assert_eq!(mode, NativeRenewalMode::ReconcileOnly);
            assert!(start_permit.is_none());
        }
        let key = rows::generation_key_parts(
            &action.fence().native_owner_id,
            &action.fence().account_id_hash,
            &action.fence().source_native_generation,
        )?;
        let row = authority.store().read(&key).await?.ok_or("generation")?;
        let generation: rows::GenerationEffect = serde_json::from_value(row.json_value()?.clone())?;
        assert_eq!(generation.operation_id, "source-operation");
        Ok(())
    })
}
