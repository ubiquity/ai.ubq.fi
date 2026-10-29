use super::support::{Host, NOW};
use gateway_auth_contracts::types::{AuthConfig, AuthRequest};
use gateway_auth_passport_sessions::sessions::PassportSessions;
use gateway_authority_actor_currentness::{
    app::ActorService, source_actor::SourceNativeRenewalActor,
};
use gateway_contracts::identity::{PrincipalId, RequestContext, RequestId};
use gateway_core::state::AuthorityStore;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
pub(super) async fn actor<'a, S: AuthorityStore>(
    store: &'a S,
    host: &'a Host,
    config: &'a AuthConfig,
    admin: bool,
) -> Result<(SourceNativeRenewalActor<'a, S>, String), GatewayError> {
    let grant = SourceJson::parse(&format!(
        r#"{{"subject":"\ud800\u0000","clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback","createdAt":"10/27/2026","token":"uos_app_session_{}"}}"#,
        "A".repeat(43)
    ))?;
    let token = PassportSessions { store, host }
        .create(&grant, &JsString16::from_units(vec![0xd800, 0]), None)
        .await?;
    let request = AuthRequest {
        method: "POST".into(),
        url: "https://ai.ubq.fi/v1/responses".into(),
        headers: std::collections::BTreeMap::from([(
            "cookie".into(),
            format!("__Host-uos_ai_passport_session={token}"),
        )]),
        ..Default::default()
    };
    let context = RequestContext {
        request_id: RequestId("callback-original".into()),
        principal: PrincipalId::from_source(JsString16::from_units(
            "passport-user:".encode_utf16().chain([0xd800, 0]).collect(),
        )),
        key_id: None,
        route: "live.sideband".into(),
        started_at_ms: NOW,
    };
    let service = ActorService::new(store, host, config);
    let actor = if admin {
        service
            .authenticate_source_admin_actor(&request, &context)
            .await?
    } else {
        service
            .authenticate_source_client_actor(&request, &context)
            .await?
    };
    Ok((actor, token))
}
