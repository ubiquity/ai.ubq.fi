mod support;
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_authority_native_live::SourceNativeLiveService;
use gateway_authority_native_records::live::{ConnectionRow, custody_key, mapping_key};
use gateway_core::state::AuthorityStore;
use gateway_native_authority_request::live::NativeLiveAuthorityRequest as Request;
use gateway_native_authority_wire::{
    live::NativeLiveRetirement, source_response::SourceNativeLiveAuthorityResponseV2 as Response,
};
use gateway_source::js_string::JsString16;
use support::{Fixture, Store, TestResult, run, seed};
fn authorize(action: &str) -> Request {
    Request::Authorize {
        v: 1,
        action_ref: action.into(),
        connection_id: "connection".into(),
    }
}
fn retire(action: &str, authorization: &str, status: u16) -> Request {
    Request::Retire {
        v: 1,
        action_ref: action.into(),
        connection_id: "connection".into(),
        authorization_ref: authorization.into(),
        upstream_status: status,
    }
}
fn fixture() -> Result<Fixture, gateway_source::error::GatewayError> {
    Fixture::new(JsString16::from_units(vec![0xd800, 0]))
}
#[test]
fn owned_404_and_410_retirement_needs_no_new_actor_mapping_credential_or_owner_admission()
-> TestResult {
    run(async {
        for status in [404, 410] {
            let f = fixture()?;
            let (actor, token) = f.actor(false).await?;
            let action = f.service().prepare_live_sideband(&actor, "call").await?;
            let Response::Authorize {
                authorization_ref, ..
            } = f.call(authorize(action.action_ref())).await?
            else {
                return Err("authorize".into());
            };
            for key in [
                session_key(&token),
                gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                gateway_authority_keys::pool(),
                gateway_authority_keys::paid_control_owner(),
            ] {
                f.store.rows.borrow_mut().remove(&key.0);
            }
            seed(
                &f.store,
                mapping_key("call", false),
                "newer",
                serde_json::json!({"account_id":"different","principal_id":"different"}),
            )?;
            let calls = f.host.calls.get();
            for _ in 0..2 {
                assert!(matches!(
                    f.call(retire(action.action_ref(), &authorization_ref, status))
                        .await?,
                    Response::Retire {
                        v: 2,
                        outcome: NativeLiveRetirement::Retired
                    }
                ));
            }
            assert_eq!(f.host.calls.get(), calls);
            assert!(f.store.read(&mapping_key("call", false)).await?.is_none());
            assert!(f.store.read(&mapping_key("call", true)).await?.is_none());
            let row = f
                .store
                .read(&custody_key("connection", "connection"))
                .await?
                .ok_or("connection")?;
            let connection: ConnectionRow = serde_json::from_value(row.json_value()?.clone())?;
            assert_eq!(connection.v, 1);
            assert_eq!(connection.upstream_status, Some(status));
            assert!(connection.retired);
            assert!(
                f.call(retire(
                    action.action_ref(),
                    &authorization_ref,
                    if status == 404 { 410 } else { 404 }
                ))
                .await
                .is_err()
            );
        }
        Ok(())
    })
}
#[test]
fn owned_retirement_preserves_unknown_pending_marker_and_unconditional_delete_retry_order()
-> TestResult {
    run(async {
        for fault in 1..=3 {
            let f = fixture()?;
            let (actor, _) = f.actor(false).await?;
            let action = f.service().prepare_live_sideband(&actor, "call").await?;
            let Response::Authorize {
                authorization_ref, ..
            } = f.call(authorize(action.action_ref())).await?
            else {
                return Err("authorize".into());
            };
            f.store.failure.set(fault);
            let Response::Retire { outcome, .. } = f
                .call(retire(action.action_ref(), &authorization_ref, 410))
                .await?
            else {
                return Err("retire".into());
            };
            assert_eq!(
                outcome,
                if fault == 1 {
                    NativeLiveRetirement::Unknown
                } else {
                    NativeLiveRetirement::Pending
                }
            );
            assert!(matches!(
                f.call(retire(action.action_ref(), &authorization_ref, 410))
                    .await?,
                Response::Retire {
                    outcome: NativeLiveRetirement::Retired,
                    ..
                }
            ));
        }
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        let action = f.service().prepare_live_sideband(&actor, "call").await?;
        let Response::Authorize {
            authorization_ref, ..
        } = f.call(authorize(action.action_ref())).await?
        else {
            return Err("authorize".into());
        };
        seed(
            &f.store,
            mapping_key("call", true),
            "malformed",
            serde_json::json!({"schema_version":2}),
        )?;
        assert!(matches!(
            f.call(retire(action.action_ref(), &authorization_ref, 404))
                .await?,
            Response::Retire {
                outcome: NativeLiveRetirement::Unknown,
                ..
            }
        ));
        assert!(f.store.read(&mapping_key("call", false)).await?.is_some());
        Ok(())
    })
}
#[test]
fn foreign_store_actor_missing_trusted_clock_and_original_bad_call_ids_cannot_prepare() -> TestResult
{
    run(async {
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        let foreign = Store::default();
        let service = SourceNativeLiveService::new(
            &foreign,
            &f.tokens,
            Some(&f.host),
            &f.id,
            &f.host,
            &f.config,
        );
        assert!(service.prepare_live_sideband(&actor, "call").await.is_err());
        assert!(foreign.rows.borrow().is_empty());
        let service =
            SourceNativeLiveService::new(&f.store, &f.tokens, None, &f.id, &f.host, &f.config);
        assert!(service.prepare_live_sideband(&actor, "call").await.is_err());
        for call in [String::new(), "x".repeat(4097)] {
            assert!(
                f.service()
                    .prepare_live_sideband(&actor, &call)
                    .await
                    .is_err()
            );
        }
        Ok(())
    })
}
