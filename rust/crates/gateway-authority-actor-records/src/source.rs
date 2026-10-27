//! Explicit actor2 DATA. The only central role/currentness proof belongs to the effect owner.
use crate::{scope::NativeActorScope, versions};
use gateway_auth_passport_contracts::actor_data::PassportActorDataV2;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_core::identity::PrincipalId;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StoredNativeActorV2 {
    pub v: u8,
    pub principal: PrincipalId,
    pub scope: NativeActorScope,
    pub config_digest: String,
    pub owner_revision: Option<String>,
    pub config_revision: Option<String>,
    pub runtime_revision: Option<String>,
    /// Minimum actual observed resource expiry; never an invented app-session lifetime.
    pub source_valid_until_ms: Option<i64>,
    pub source: PassportActorDataV2,
}
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Envelope {
    v: u8,
    principal: JsString16,
    scope: NativeActorScope,
    config_digest: String,
    owner_revision: Option<String>,
    config_revision: Option<String>,
    runtime_revision: Option<String>,
    #[serde(deserialize_with = "required_expiry")]
    source_valid_until_ms: Option<i64>,
    source: SourceJson,
}
fn required_expiry<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<i64>, D::Error> {
    Option::<i64>::deserialize(d)
}
impl StoredNativeActorV2 {
    /// Typed source JSON, not legacy PrincipalId serde and not a reusable grant.
    pub fn to_source_json(&self) -> Result<SourceJson, GatewayError> {
        if self.v != versions::STORED_ACTOR_V2 || !self.principal_matches_source() {
            return Err(invalid());
        }
        SourceJson::from_serializable(&Envelope {
            v: self.v,
            principal: self.principal.as_source().clone(),
            scope: self.scope,
            config_digest: self.config_digest.clone(),
            owner_revision: self.owner_revision.clone(),
            config_revision: self.config_revision.clone(),
            runtime_revision: self.runtime_revision.clone(),
            source_valid_until_ms: self.source_valid_until_ms,
            source: self.source.to_source_json()?,
        })
    }
    /// Version-first RawValue decoding retains lone units and refuses mixed/unknown source shapes.
    pub fn from_source_json(value: &SourceJson) -> Result<Self, GatewayError> {
        let raw: Envelope = value.deserialize().map_err(|_| invalid())?;
        if raw.v != versions::STORED_ACTOR_V2 {
            return Err(invalid());
        }
        let value = Self {
            v: raw.v,
            principal: PrincipalId::from_source(raw.principal),
            scope: raw.scope,
            config_digest: raw.config_digest,
            owner_revision: raw.owner_revision,
            config_revision: raw.config_revision,
            runtime_revision: raw.runtime_revision,
            source_valid_until_ms: raw.source_valid_until_ms,
            source: PassportActorDataV2::from_source_json(&raw.source)?,
        };
        if !value.principal_matches_source() {
            return Err(invalid());
        }
        Ok(value)
    }
    pub fn framed_bytes(&self) -> Result<Vec<u8>, GatewayError> {
        let mut bytes = versions::ACTOR_POLICY_V2_DOMAIN.to_vec();
        bytes.extend_from_slice(self.to_source_json()?.as_bytes());
        Ok(bytes)
    }
    pub fn policy_digest(&self) -> Result<String, GatewayError> {
        Ok(sha256_hex(&self.framed_bytes()?))
    }
    /// JWT expiry remains mandatory; a permanent app supplies no fabricated expiry.
    pub fn source_expiry(&self) -> Option<i64> {
        match &self.source {
            PassportActorDataV2::PassportAppSession(_) => None,
            PassportActorDataV2::PassportJwt(value) => Some(value.expires_at_ms),
        }
    }
    /// Exact source identity DATA; it does not consult role, date, store or currentness.
    pub fn principal_matches_source(&self) -> bool {
        let subject = match &self.source {
            PassportActorDataV2::PassportAppSession(value) => &value.subject,
            PassportActorDataV2::PassportJwt(value) => &value.sub,
        };
        self.principal
            .as_source()
            .as_units()
            .iter()
            .copied()
            .eq("passport-user:"
                .encode_utf16()
                .chain(subject.as_units().iter().copied()))
    }
}
fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Versioned native actor DATA is invalid.",
        "native_actor_data_invalid",
    )
}
