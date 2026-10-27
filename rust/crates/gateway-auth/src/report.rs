//! Auth-report DATA, preserving the existing schema and repeated-cookie transport.
use gateway_auth_contracts::types::AuthPrincipal;
use gateway_core::identity::PrincipalId;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
#[derive(Serialize)]
struct Report<'a> {
    ok: bool,
    service: &'static str,
    auth: Auth<'a>,
}
#[derive(Serialize)]
struct Auth<'a> {
    mode: &'a str,
    is_admin: bool,
    is_super_admin: bool,
    principal: &'a JsString16,
    method: &'a SourceJson,
    token: Token<'a>,
}
#[derive(Serialize)]
struct Token<'a> {
    present: bool,
    length: Option<usize>,
    shape: Option<&'a str>,
    sha256_12: Option<String>,
}
pub(crate) fn document(
    mode: &str,
    auth: &AuthPrincipal,
    principal: &PrincipalId,
    method: &SourceJson,
    token: Option<&str>,
    shape: Option<&str>,
) -> Result<SourceJson, GatewayError> {
    SourceJson::from_serializable(&Report {
        ok: true,
        service: "ai.ubq.fi",
        auth: Auth {
            mode,
            is_admin: auth.is_admin,
            is_super_admin: auth.is_super_admin,
            principal: principal.as_source(),
            method,
            token: Token {
                present: token.is_some(),
                length: token.map(str::len),
                shape,
                sha256_12: token.map(|t| {
                    gateway_auth_primitives::crypto::sha256_hex(t.as_bytes())[..12].into()
                }),
            },
        },
    })
}
