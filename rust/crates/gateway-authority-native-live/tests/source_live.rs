mod support;
use gateway_auth_passport_sessions::sessions::session_key;
use gateway_authority_native_records::live::{custody_key, mapping_key};
use gateway_core::{
    source_control::{decode_source_control_record, encode_source_control_record},
    state::AuthorityStore,
};
use gateway_native_authority_request::live::NativeLiveAuthorityRequest as Request;
use gateway_native_authority_wire::source_access::NativeLiveAccessWire;
use gateway_native_authority_wire::source_response::SourceNativeLiveAuthorityResponseV2 as Response;
use gateway_source::{js_string::JsString16, source_json::SourceJson};
use std::{
    future::Future,
    task::{Context, Waker},
};
use support::{Fixture, NOW, Nonces, TestResult, run, signed, source};
fn authorize(action: &str) -> Request {
    Request::Authorize {
        v: 1,
        action_ref: action.into(),
        connection_id: "connection".into(),
    }
}
fn recheck(action: &str, authorization: &str) -> Request {
    Request::Recheck {
        v: 1,
        action_ref: action.into(),
        connection_id: "connection".into(),
        authorization_ref: authorization.into(),
    }
}
fn fixture() -> Result<Fixture, gateway_source::error::GatewayError> {
    Fixture::new(JsString16::from_units(vec![0xd800, 0]))
}
#[test]
fn actual_c_live2_access_only_source_units_signed_reply_and_current_credential_recheck()
-> TestResult {
    run(async {
        for units in [
            vec![0xd800, 0],
            vec![0xdc00],
            vec![0xfffd],
            vec![0],
            vec![b'x' as u16; 500],
        ] {
            let f = Fixture::new(JsString16::from_units(units))?;
            let (actor, _) = f.actor(false).await?;
            let service = f.service();
            let action = service.prepare_live_sideband(&actor, "call").await?;
            let body = serde_json::to_vec(&authorize(action.action_ref()))?;
            let reply = service.execute(&f.proof(&body).await?, &body).await?;
            let raw = SourceJson::from_serializable(&reply)?;
            let response_nonce = gateway_auth_primitives::crypto::base64url(&[90; 32]);
            let signature =
                f.id.sign_live_authority_response(&response_nonce, &body, 200, raw.as_bytes())?;
            f.id.verify_live_authority_response(
                &response_nonce,
                &body,
                200,
                raw.as_bytes(),
                &signature,
            )?;
            let wire: Response<NativeLiveAccessWire> = raw.deserialize()?;
            let Response::Authorize {
                v: 2,
                fence: wire_fence,
                access: wire_access,
                ..
            } = wire
            else {
                return Err("wire response version".into());
            };
            assert_eq!(&wire_fence.principal, f.principal().as_source());
            assert_eq!(wire_fence.v, 2);
            assert_eq!(wire_access.expose(), "SYNTHETIC_ACCESS");
            let mut altered = raw.as_bytes().to_vec();
            altered.push(b' ');
            assert!(
                f.id.verify_live_authority_response(
                    &response_nonce,
                    &body,
                    200,
                    &altered,
                    &signature
                )
                .is_err()
            );
            let decoded: Response<serde_json::Value> = raw.deserialize()?;
            let Response::Authorize { v: 2, fence, .. } = decoded else {
                return Err("response version".into());
            };
            assert_eq!(&fence.principal, f.principal().as_source());
            assert_eq!(fence.v, 2);
            let Response::Authorize {
                authorization_ref,
                access,
                ..
            } = reply
            else {
                return Err("authorize".into());
            };
            assert_eq!(
                serde_json::to_string(&access)?,
                r#"{"access_token":"SYNTHETIC_ACCESS"}"#
            );
            assert_eq!(access.expose(), "SYNTHETIC_ACCESS");
            source(&f.store, "CURRENT_ACCESS", "source-two")?;
            let calls = f.host.calls.get();
            let Response::Recheck {
                v: 2,
                access,
                fence,
            } = f
                .call(recheck(action.action_ref(), &authorization_ref))
                .await?
            else {
                return Err("recheck".into());
            };
            assert!(f.host.calls.get() > calls);
            assert_eq!(access.expose(), "CURRENT_ACCESS");
            assert_eq!(fence.source_revision, "source-two");
            for key in [
                custody_key("action", action.action_ref()),
                custody_key("connection", "connection"),
            ] {
                let row = f.store.read(&key).await?.ok_or("custody")?;
                let data = decode_source_control_record(&row)?;
                assert!(!data.as_str().contains("CURRENT_ACCESS"));
                assert!(!data.as_str().contains("SYNTHETIC_REFRESH"));
                assert!(!data.as_str().contains("uos_app_session_"));
            }
        }
        Ok(())
    })
}
#[test]
fn fresh_client_role_remains_eligible_while_real_admin_actor_cannot_prepare_a_live_action()
-> TestResult {
    run(async {
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        f.host.admin.set(false);
        let action = f.service().prepare_live_sideband(&actor, "call").await?;
        assert!(matches!(
            f.call(authorize(action.action_ref())).await?,
            Response::Authorize { v: 2, .. }
        ));
        let admin = fixture()?;
        let (actor, _) = admin.actor(true).await?;
        let calls = admin.host.calls.get();
        assert!(
            admin
                .service()
                .prepare_live_sideband(&actor, "call")
                .await
                .is_err()
        );
        assert_eq!(admin.host.calls.get(), calls);
        Ok(())
    })
}
#[test]
fn c_503_retains_local_session_and_authoritative_401_deletes_before_any_connection_effect()
-> TestResult {
    run(async {
        let f = fixture()?;
        let (actor, token) = f.actor(false).await?;
        f.host.status.set(503);
        let error = f
            .service()
            .prepare_live_sideband(&actor, "call")
            .await
            .err()
            .ok_or("503 accepted")?;
        assert_eq!(error.status, 503);
        assert!(f.store.read(&session_key(&token)).await?.is_some());
        f.host.status.set(200);
        let action = f.service().prepare_live_sideband(&actor, "call").await?;
        f.host.status.set(401);
        assert_eq!(
            f.call(authorize(action.action_ref()))
                .await
                .err()
                .ok_or("401 accepted")?
                .status,
            401
        );
        assert!(f.store.read(&session_key(&token)).await?.is_none());
        assert!(
            f.store
                .read(&custody_key("connection", "connection"))
                .await?
                .is_none()
        );
        Ok(())
    })
}
#[test]
fn logout_during_each_actual_c_await_refuses_prepare_authorize_and_recheck() -> TestResult {
    run(async {
        for stage in 0..3 {
            let f = fixture()?;
            let (actor, token) = f.actor(false).await?;
            let service = f.service();
            let action = if stage > 0 {
                Some(service.prepare_live_sideband(&actor, "call").await?)
            } else {
                None
            };
            let reference = action.as_ref().map(|a| a.action_ref()).unwrap_or("unused");
            let authorization = if stage == 2 {
                let Response::Authorize {
                    authorization_ref, ..
                } = f.call(authorize(reference)).await?
                else {
                    return Err("authorize".into());
                };
                authorization_ref
            } else {
                String::new()
            };
            let body = serde_json::to_vec(&if stage == 2 {
                recheck(reference, &authorization)
            } else {
                authorize(reference)
            })?;
            let proof = f.proof(&body).await?;
            let before = f
                .store
                .read(&custody_key("connection", "connection"))
                .await?;
            f.host.hold.set(true);
            let mut future = Box::pin(async {
                if stage == 0 {
                    service
                        .prepare_live_sideband(&actor, "call")
                        .await
                        .map(|_| ())
                } else {
                    service.execute(&proof, &body).await.map(|_| ())
                }
            });
            let mut cx = Context::from_waker(Waker::noop());
            assert!(future.as_mut().poll(&mut cx).is_pending());
            f.store.rows.borrow_mut().remove(&session_key(&token).0);
            f.host.hold.set(false);
            assert!(future.await.is_err());
            assert_eq!(
                f.store
                    .read(&custody_key("connection", "connection"))
                    .await?
                    .map(|r| r.revision),
                before.map(|r| r.revision)
            );
            if stage == 0 {
                assert!(
                    !f.store
                        .rows
                        .borrow()
                        .contains_key(&custody_key("action", "source-live-1").0)
                );
            }
        }
        Ok(())
    })
}
#[test]
fn logout_at_each_original_final_cas_cannot_commit_action_connection_or_recheck() -> TestResult {
    run(async {
        for stage in 0..3 {
            let f = fixture()?;
            let (actor, token) = f.actor(false).await?;
            let service = f.service();
            let action = if stage > 0 {
                Some(service.prepare_live_sideband(&actor, "call").await?)
            } else {
                None
            };
            let reference = action.as_ref().map(|a| a.action_ref()).unwrap_or("unused");
            let authorization = if stage == 2 {
                let Response::Authorize {
                    authorization_ref, ..
                } = f.call(authorize(reference)).await?
                else {
                    return Err("authorize".into());
                };
                authorization_ref
            } else {
                String::new()
            };
            let before = f
                .store
                .read(&custody_key("connection", "connection"))
                .await?;
            *f.store.race.borrow_mut() = Some((session_key(&token), None));
            if stage == 0 {
                assert!(service.prepare_live_sideband(&actor, "call").await.is_err());
                assert!(
                    !f.store
                        .rows
                        .borrow()
                        .contains_key(&custody_key("action", "source-live-1").0)
                );
            } else {
                assert!(
                    f.call(if stage == 2 {
                        recheck(reference, &authorization)
                    } else {
                        authorize(reference)
                    })
                    .await
                    .is_err()
                );
            }
            assert_eq!(
                f.store
                    .read(&custody_key("connection", "connection"))
                    .await?
                    .map(|r| r.revision),
                before.map(|r| r.revision)
            );
        }
        Ok(())
    })
}
#[test]
fn dropping_pending_c_and_expiring_actual_native_proof_before_final_cas_leave_no_connection()
-> TestResult {
    run(async {
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        let service = f.service();
        let action = service.prepare_live_sideband(&actor, "call").await?;
        let body = serde_json::to_vec(&authorize(action.action_ref()))?;
        let proof = f.proof(&body).await?;
        f.host.hold.set(true);
        let mut future = Box::pin(service.execute(&proof, &body));
        let mut cx = Context::from_waker(Waker::noop());
        assert!(future.as_mut().poll(&mut cx).is_pending());
        drop(future);
        assert!(
            f.store
                .read(&custody_key("connection", "connection"))
                .await?
                .is_none()
        );
        let mut future = Box::pin(service.execute(&proof, &body));
        assert!(future.as_mut().poll(&mut cx).is_pending());
        f.host
            .now
            .set(NOW + gateway_auth_native_wire::native_read::NATIVE_AUTH_WINDOW_MS + 1);
        f.host.hold.set(false);
        assert!(future.await.is_err());
        assert!(
            f.store
                .read(&custody_key("connection", "connection"))
                .await?
                .is_none()
        );
        Ok(())
    })
}
#[test]
fn actual_native_peer_body_replay_and_mixed_source_row_refuse_before_authorization() -> TestResult {
    run(async {
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        let action = f.service().prepare_live_sideband(&actor, "call").await?;
        let body = serde_json::to_vec(&authorize(action.action_ref()))?;
        let proof = f.proof(&body).await?;
        let mut altered = body.clone();
        altered.push(b' ');
        assert!(f.service().execute(&proof, &altered).await.is_err());
        let other = fixture()?;
        let foreign = gateway_auth_native_proof::native_auth::NativeTransportIdentity::new(
            "other".into(),
            "other".into(),
            "other".into(),
            [74; 32],
            std::collections::BTreeSet::from([
                gateway_auth_native_contracts::transport::NativeCapability::LiveSideband,
            ]),
        )?;
        let wrong = signed(&foreign, &other.nonces, &body, 30, NOW).await?;
        assert!(f.service().execute(&wrong, &body).await.is_err());
        let nonces = Nonces::default();
        let _ = signed(&f.id, &nonces, &body, 31, NOW).await?;
        assert!(signed(&f.id, &nonces, &body, 31, NOW).await.is_err());
        let key = custody_key("action", action.action_ref());
        let original = f.store.read(&key).await?.ok_or("action")?;
        let raw = decode_source_control_record(&original)?;
        let mixed = SourceJson::parse(&raw.as_str().replacen("\"v\":2", "\"v\":1", 1))?;
        f.store
            .rows
            .borrow_mut()
            .insert(key.0, encode_source_control_record("mixed", &mixed, None)?);
        assert!(f.call(authorize(action.action_ref())).await.is_err());
        assert!(
            f.store
                .read(&custody_key("connection", "connection"))
                .await?
                .is_none()
        );
        Ok(())
    })
}
#[test]
fn mapping_source_normalized_and_owner_races_are_retained_in_the_final_effect_cas() -> TestResult {
    run(async {
        for kind in 0..4 {
            let f = fixture()?;
            let (actor, _) = f.actor(false).await?;
            let action = f.service().prepare_live_sideband(&actor, "call").await?;
            let key = match kind {
                0 => mapping_key("call", false),
                1 => gateway_auth_credentials::provider_credentials::codex_source_pool_key(),
                2 => gateway_authority_keys::pool(),
                _ => gateway_authority_keys::paid_control_owner(),
            };
            *f.store.race.borrow_mut() = Some((key, None));
            assert!(f.call(authorize(action.action_ref())).await.is_err());
            assert!(
                f.store
                    .read(&custody_key("connection", "connection"))
                    .await?
                    .is_none()
            );
        }
        let f = fixture()?;
        let (actor, _) = f.actor(false).await?;
        let key = mapping_key("call", false);
        let row = f.store.read(&key).await?.ok_or("mapping")?;
        let raw = decode_source_control_record(&row)?;
        f.store.rows.borrow_mut().insert(
            key.0,
            encode_source_control_record("expired", &raw, Some(NOW))?,
        );
        assert!(
            f.service()
                .prepare_live_sideband(&actor, "call")
                .await
                .is_err()
        );
        Ok(())
    })
}
