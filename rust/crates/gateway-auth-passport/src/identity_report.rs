//! Existing public Passport method projection, retaining exact source units.
use gateway_auth_passport_contracts::{
    actor_data::PassportSessionData, session::PassportSourceClaims,
};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
#[derive(Serialize)]
struct User<'a> {
    sub: &'a JsString16,
    handle: &'a JsString16,
    is_admin: bool,
    scopes: &'a [JsString16],
    auth: &'a JsString16,
    exp: Option<i64>,
    jti: Option<&'a JsString16>,
    agent_token_id: Option<&'a JsString16>,
}
#[derive(Serialize)]
struct Method<'a> {
    kind: &'static str,
    principal: JsString16,
    user: User<'a>,
}
fn principal(sub: &JsString16) -> JsString16 {
    let mut units = "passport-user:".encode_utf16().collect::<Vec<_>>();
    units.extend_from_slice(sub.as_units());
    JsString16::from_units(units)
}
pub fn jwt(claims: &PassportSourceClaims, bearer: bool) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(&Method {
        kind: if bearer {
            "passport_bearer"
        } else {
            "passport_session"
        },
        principal: principal(&claims.sub),
        user: User {
            sub: &claims.sub,
            handle: &claims.handle,
            is_admin: claims.is_admin,
            scopes: &claims.scopes,
            auth: &claims.auth,
            exp: Some(claims.exp),
            jti: Some(&claims.jti),
            agent_token_id: claims.agent_token_id.as_ref(),
        },
    })
}
pub fn session(value: &PassportSessionData) -> Result<SourceJson, GatewayError> {
    match value {
        PassportSessionData::Jwt(claims) => jwt(claims, false),
        PassportSessionData::App(data) => {
            let identity = &data.identity;
            let auth = JsString16::from_utf8("sso");
            SourceJson::from_serializable(&Method {
                kind: "passport_session",
                principal: principal(&identity.subject),
                user: User {
                    sub: &identity.subject,
                    handle: &identity.handle,
                    is_admin: identity.is_admin,
                    scopes: &identity.scopes,
                    auth: &auth,
                    exp: None,
                    jti: None,
                    agent_token_id: None,
                },
            })
        }
    }
}
