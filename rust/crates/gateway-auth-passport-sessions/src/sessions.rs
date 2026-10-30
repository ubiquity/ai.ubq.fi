use gateway_auth_contracts::{types::AuthHost, upstream::AuthUpstreamRequest};
use gateway_auth_passport_contracts::{
    actor_data::{PassportAppSessionBinding, PassportAppSessionData},
    passport::PassportConfig,
    session::{PassportDate, PassportSessionIdentity},
};
use gateway_auth_primitives::{
    crypto::{Secret, base64url, sha256_hex},
    store,
};
use gateway_core::{
    source_control,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordMutation, StoredRecord},
};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::tuple_key::{ScalarKeyPart, TupleKey};
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::{Deserialize, Serialize};

pub fn token_shape(token: &str) -> bool {
    suffix(token, "uos_ai_passport_session_")
}
fn suffix(token: &str, prefix: &str) -> bool {
    token.strip_prefix(prefix).is_some_and(|tail| {
        tail.len() == 43
            && tail
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-'))
    })
}
fn bounded(value: &JsString16, max: usize, empty: bool) -> bool {
    (empty || !value.as_units().is_empty()) && value.as_units().len() <= max
}
pub fn session_key(token: &str) -> RecordKey {
    digest_key(&sha256_hex(token.as_bytes()))
}
fn digest_key(digest: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("auth"),
        ScalarKeyPart::String("passport"),
        ScalarKeyPart::String("sessions"),
        ScalarKeyPart::String(digest),
    ])
    .to_record_key()
}
pub fn unavailable() -> GatewayError {
    GatewayError::new(
        503,
        "Passport session verification is unavailable.",
        Some("passport_unavailable"),
    )
}
fn unauthorized() -> GatewayError {
    gateway_auth_primitives::error::unauthorized()
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct Binding {
    client_id: String,
    audience: String,
    origin: String,
    redirect_uri: String,
}
impl Binding {
    fn valid(&self, fixed: &PassportConfig) -> bool {
        fixed.validated().is_some()
            && self.client_id == fixed.client_id
            && self.audience == fixed.audience
            && self.origin == fixed.origin
            && self.redirect_uri == fixed.redirect_uri
    }
}
// Flatten buffers would lose lone UTF16 units. Binding fields are declared
// directly; each source string decodes from RawValue through JsString16.
#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
struct Record {
    version: f64,
    subject: JsString16,
    client_id: String,
    audience: String,
    origin: String,
    redirect_uri: String,
    created_at: JsString16,
    app_session_token: String,
}
impl Record {
    fn binding(&self, session_digest: String, revision: &str) -> PassportAppSessionBinding {
        PassportAppSessionBinding {
            session_digest,
            row_revision: revision.into(),
            subject: self.subject.clone(),
            created_at: self.created_at.clone(),
            app_token_digest: sha256_hex(self.app_session_token.as_bytes()),
            client_id: self.client_id.clone(),
            audience: self.audience.clone(),
            origin: self.origin.clone(),
            redirect_uri: self.redirect_uri.clone(),
        }
    }
    fn valid<H: PassportDate>(&self, host: &H, config: &PassportConfig) -> bool {
        self.version == 1.0
            && bounded(&self.subject, 500, false)
            && bounded(&self.created_at, 100, false)
            && host.parse_date_ms(&self.created_at).is_finite()
            && suffix(&self.app_session_token, "uos_app_session_")
            && Binding {
                client_id: self.client_id.clone(),
                audience: self.audience.clone(),
                origin: self.origin.clone(),
                redirect_uri: self.redirect_uri.clone(),
            }
            .valid(config)
    }
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct Grant {
    subject: JsString16,
    client_id: String,
    audience: String,
    origin: String,
    redirect_uri: String,
    created_at: JsString16,
    token: String,
}
impl Grant {
    fn record(self) -> Record {
        Record {
            version: 1.0,
            subject: self.subject,
            client_id: self.client_id,
            audience: self.audience,
            origin: self.origin,
            redirect_uri: self.redirect_uri,
            created_at: self.created_at,
            app_session_token: self.token,
        }
    }
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CentralSession {
    subject: JsString16,
    client_id: String,
    audience: String,
    origin: String,
    redirect_uri: String,
    created_at: JsString16,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct CentralUser {
    id: JsString16,
    handle: JsString16,
    is_admin: bool,
    scopes: Vec<JsString16>,
}
#[derive(Deserialize)]
struct Introspection {
    authenticated: bool,
    session: CentralSession,
    user: CentralUser,
}
impl Introspection {
    fn identity(
        self,
        record: &Record,
        config: &PassportConfig,
    ) -> Result<PassportSessionIdentity, GatewayError> {
        let s = self.session;
        if !self.authenticated
            || s.subject != record.subject
            || s.created_at != record.created_at
            || !(Binding {
                client_id: s.client_id,
                audience: s.audience,
                origin: s.origin,
                redirect_uri: s.redirect_uri,
            })
            .valid(config)
            || self.user.id != record.subject
            || !bounded(&self.user.handle, 500, true)
            || self.user.scopes.len() > 128
            || self.user.scopes.iter().any(|s| !bounded(s, 500, true))
        {
            return Err(unavailable());
        }
        Ok(PassportSessionIdentity {
            subject: record.subject.clone(),
            handle: self.user.handle,
            is_admin: self.user.is_admin,
            scopes: self.user.scopes,
        })
    }
}

pub struct PassportSessions<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
}
impl<S: AuthorityStore, H: AuthHost + PassportDate> PassportSessions<'_, S, H> {
    fn decode(&self, row: &StoredRecord, config: &PassportConfig) -> Result<Record, GatewayError> {
        if row.expires_at_ms.is_some() {
            return Err(unavailable());
        }
        let document =
            source_control::decode_source_control_record(row).map_err(|_| unavailable())?;
        let record: Record = document.deserialize().map_err(|_| unavailable())?;
        if !record.valid(self.host, config) {
            return Err(unavailable());
        }
        Ok(record)
    }
    async fn request(
        &self,
        record: &Record,
        revoke: bool,
        config: &PassportConfig,
    ) -> Result<gateway_auth_contracts::upstream::AuthUpstreamResponse, GatewayError> {
        self.host
            .fetch_auth(if revoke {
                AuthUpstreamRequest::PassportRevoke {
                    app_session_token: Secret::new(record.app_session_token.clone()),
                    binding: config.clone(),
                }
            } else {
                AuthUpstreamRequest::PassportSession {
                    app_session_token: Secret::new(record.app_session_token.clone()),
                    binding: config.clone(),
                }
            })
            .await
            .map_err(|_| unavailable())
    }
    /// Validate the complete grant before touching prior authority. Creation and
    /// optional replacement are one atomic transition, never delete-then-create.
    pub async fn create(
        &self,
        grant: &SourceJson,
        subject: &JsString16,
        previous: Option<&str>,
    ) -> Result<String, GatewayError> {
        self.create_for(grant, subject, previous, &PassportConfig::production())
            .await
    }
    pub async fn create_for(
        &self,
        grant: &SourceJson,
        subject: &JsString16,
        previous: Option<&str>,
        config: &PassportConfig,
    ) -> Result<String, GatewayError> {
        let record = gateway_source::source_serialization::canonical(grant)?
            .deserialize::<Grant>()
            .map_err(|_| unavailable())?
            .record();
        if &record.subject != subject || !record.valid(self.host, config) {
            return Err(unavailable());
        }
        let bytes = self.host.random_bytes(32).map_err(|_| unavailable())?;
        if bytes.len() != 32 {
            return Err(unavailable());
        }
        let token = format!("uos_ai_passport_session_{}", base64url(&bytes));
        let key = session_key(&token);
        let mut previous_key = previous.filter(|token| token_shape(token)).map(session_key);
        let previous_record = match &previous_key {
            Some(key) => self.store.read(key).await.map_err(|_| unavailable())?,
            None => None,
        };
        if config.http_loopback()
            && previous_record
                .as_ref()
                .is_some_and(|row| self.decode(row, config).is_err())
        {
            previous_key = None;
        }
        let revision = sha256_hex(format!("passport-session-v1\0{token}").as_bytes());
        let document = SourceJson::from_serializable(&record).map_err(|_| unavailable())?;
        let stored = source_control::encode_source_control_record(revision, &document, None)
            .map_err(|_| unavailable())?;
        let mut mutations = vec![RecordMutation::Put {
            key: key.clone(),
            record: stored,
        }];
        if let Some(key) = previous_key {
            mutations.push(RecordMutation::Delete { key });
        }
        let outcome = self
            .store
            .commit(AtomicTransition {
                checks: vec![store::check(&key, None)],
                mutations,
            })
            .await
            .map_err(|_| unavailable())?;
        if outcome != CommitOutcome::Committed {
            return Err(unavailable());
        }
        if let Some(previous) = previous_record
            .as_ref()
            .and_then(|row| self.decode(row, config).ok())
        {
            let _ = self.request(&previous, true, config).await;
        }
        Ok(token)
    }
    // The source's unconditional KV deletion is a finite mutation in the common
    // authority, not a reconstructed write grant or an expiring session index.
    async fn delete(&self, key: RecordKey) -> Result<(), GatewayError> {
        let outcome = self
            .store
            .commit(AtomicTransition {
                checks: Vec::new(),
                mutations: vec![RecordMutation::Delete { key }],
            })
            .await
            .map_err(|_| unavailable())?;
        if outcome != CommitOutcome::Committed {
            return Err(unavailable());
        }
        Ok(())
    }
    pub async fn get(&self, token: &str) -> Result<PassportSessionIdentity, GatewayError> {
        Ok(self.authenticate(token).await?.identity)
    }
    /// Authenticates the actual cookie and returns identity plus its exact source binding DATA.
    pub async fn authenticate(&self, token: &str) -> Result<PassportAppSessionData, GatewayError> {
        self.authenticate_for(token, &PassportConfig::production())
            .await
    }
    pub async fn authenticate_for(
        &self,
        token: &str,
        config: &PassportConfig,
    ) -> Result<PassportAppSessionData, GatewayError> {
        if !token_shape(token) {
            return Err(unauthorized());
        }
        let digest = sha256_hex(token.as_bytes());
        let key = digest_key(&digest);
        let row = self
            .store
            .read(&key)
            .await
            .map_err(|_| unavailable())?
            .ok_or_else(unauthorized)?;
        let record = self.decode(&row, config)?;
        let binding = record.binding(digest, &row.revision);
        self.introspect(key, &binding, &record, config).await
    }
    /// Concrete constructor-held preflight for Authority. Reads the stored app token;
    /// no caller verification callback or token reconstruction. Return value is DATA.
    pub async fn check_bound(
        &self,
        binding: &PassportAppSessionBinding,
    ) -> Result<PassportAppSessionData, GatewayError> {
        self.check_bound_for(binding, &PassportConfig::production())
            .await
    }
    pub async fn check_bound_for(
        &self,
        binding: &PassportAppSessionBinding,
        config: &PassportConfig,
    ) -> Result<PassportAppSessionData, GatewayError> {
        if !binding.has_valid_shape_for(config) {
            return Err(unauthorized());
        }
        let key = digest_key(&binding.session_digest);
        let row = self
            .store
            .read(&key)
            .await
            .map_err(|_| unavailable())?
            .ok_or_else(unauthorized)?;
        let record = self.decode(&row, config)?;
        if record.binding(binding.session_digest.clone(), &row.revision) != *binding {
            return Err(unauthorized());
        }
        self.introspect(key, binding, &record, config).await
    }
    async fn introspect(
        &self,
        key: RecordKey,
        binding: &PassportAppSessionBinding,
        record: &Record,
        config: &PassportConfig,
    ) -> Result<PassportAppSessionData, GatewayError> {
        let response = self.request(record, false, config).await?;
        // Authoritative 401 remains authoritative even when body cancellation fails.
        if response.status == 401 {
            self.delete(key).await?;
            return Err(unauthorized());
        }
        if !response.success() || response.body.len() > 65_536 {
            return Err(unavailable());
        }
        let document = gateway_auth_passport_contracts::session::parse_source_json(&response.body)
            .map_err(|_| unavailable())?;
        let identity = document
            .deserialize::<Introspection>()
            .map_err(|_| unavailable())?
            .identity(record, config)?;
        let row = self
            .store
            .read(&key)
            .await
            .map_err(|_| unavailable())?
            .ok_or_else(unauthorized)?;
        let current = self.decode(&row, config)?;
        if current.binding(binding.session_digest.clone(), &row.revision) != *binding {
            return Err(unauthorized());
        }
        Ok(PassportAppSessionData {
            identity,
            binding: binding.clone(),
        })
    }
    pub async fn revoke(&self, token: &str) -> Result<&'static str, GatewayError> {
        self.revoke_for(token, &PassportConfig::production()).await
    }
    pub async fn revoke_for(
        &self,
        token: &str,
        config: &PassportConfig,
    ) -> Result<&'static str, GatewayError> {
        if !token_shape(token) {
            return Ok("not_applicable");
        }
        let key = session_key(token);
        let row = self.store.read(&key).await.map_err(|_| unavailable())?;
        if config.http_loopback()
            && row
                .as_ref()
                .is_some_and(|row| self.decode(row, config).is_err())
        {
            return Ok("not_applicable");
        }
        self.delete(key).await?;
        let Some(record) = row.as_ref().and_then(|row| self.decode(row, config).ok()) else {
            return Ok("not_applicable");
        };
        Ok(match self.request(&record, true, config).await {
            Ok(response) if response.status == 204 => "revoked",
            _ => "unavailable",
        })
    }
}
