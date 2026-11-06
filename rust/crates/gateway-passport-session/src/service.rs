//! Create, introspect and revoke persistent sessions (source `passport-sessions.ts`).
use crate::{
    binding::{Binding, LOCAL_TOKEN_PREFIX, base64url, bounded, is_app_token, timestamp},
    call::{PASSPORT_SESSION_MAX_BYTES, SessionCall, SessionHost, SessionReply},
    record::{SessionRecord, session_key},
};
use gateway_core::{
    error::GatewayError,
    state::{AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation},
    state::{RevisionCheck, StoredRecord},
};
use serde_json::Value;

/// The current Passport user behind a session, read on every use.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Identity {
    pub subject: String,
    pub handle: String,
    pub is_admin: bool,
    pub scopes: Vec<String>,
}

/// Upstream result of an explicit logout, reported in
/// `X-UOS-Passport-Upstream-Revoke`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RevokeOutcome {
    Revoked,
    Unavailable,
    NotApplicable,
}

impl RevokeOutcome {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Revoked => "revoked",
            Self::Unavailable => "unavailable",
            Self::NotApplicable => "not_applicable",
        }
    }
}

/// Source `PassportSessionError(401)`: the session no longer grants authority.
pub fn unauthorized() -> GatewayError {
    GatewayError::new(401, "Unauthorized", Some("invalid_api_key"))
}

/// Source `PassportSessionError(503)`: verification is temporarily unavailable
/// and the local session is retained.
pub fn unavailable() -> GatewayError {
    GatewayError::new(
        503,
        "Passport verification is unavailable.",
        Some("passport_unavailable"),
    )
}

fn identity(body: &Value, record: &SessionRecord, binding: &Binding) -> Option<Identity> {
    let (session, user) = (body.get("session")?, body.get("user")?);
    if body.get("authenticated") != Some(&Value::Bool(true))
        || !session.is_object()
        || !user.is_object()
        || !binding.matches(session)
        || session.get("subject").and_then(Value::as_str) != Some(record.subject.as_str())
        || session.get("createdAt").and_then(Value::as_str) != Some(record.created_at.as_str())
        || user.get("id").and_then(Value::as_str) != Some(record.subject.as_str())
    {
        return None;
    }
    let handle = user
        .get("handle")?
        .as_str()
        .filter(|handle| handle.encode_utf16().count() <= 500)?;
    let scopes = user.get("scopes")?.as_array().filter(|s| s.len() <= 128)?;
    let scopes = scopes
        .iter()
        .map(|scope| {
            scope
                .as_str()
                .filter(|scope| scope.encode_utf16().count() <= 500)
                .map(str::to_owned)
        })
        .collect::<Option<Vec<_>>>()?;
    Some(Identity {
        subject: record.subject.clone(),
        handle: handle.into(),
        is_admin: user.get("isAdmin")?.as_bool()?,
        scopes,
    })
}

pub struct PassportSessions<'a, S, H> {
    pub store: &'a S,
    pub host: &'a H,
    pub binding: &'a Binding,
}

impl<S: AuthorityStore, H: SessionHost> PassportSessions<'_, S, H> {
    fn random(&self, length: usize) -> Result<Vec<u8>, GatewayError> {
        self.host
            .random_bytes(length)
            .ok()
            .filter(|bytes| bytes.len() == length)
            .ok_or_else(unavailable)
    }

    async fn read(&self, key: &RecordKey) -> Result<Option<Value>, GatewayError> {
        let row = self.store.read(key).await.map_err(|_| unavailable())?;
        row.map(|row| row.json_value().cloned().map_err(|_| unavailable()))
            .transpose()
    }

    async fn commit(
        &self,
        checks: Vec<RevisionCheck>,
        mutations: Vec<RecordMutation>,
    ) -> Result<(), GatewayError> {
        match self
            .store
            .commit(AtomicTransition { checks, mutations })
            .await
        {
            Ok(CommitOutcome::Committed) => Ok(()),
            _ => Err(unavailable()),
        }
    }

    async fn delete(&self, key: RecordKey) -> Result<(), GatewayError> {
        self.commit(Vec::new(), vec![RecordMutation::Delete { key }])
            .await
    }

    async fn upstream(
        &self,
        record: &SessionRecord,
        revoke: bool,
    ) -> Result<SessionReply, GatewayError> {
        let call = SessionCall::new(
            record.app_session_token.clone(),
            self.binding.clone(),
            revoke,
        );
        self.host.fetch_session(call).await
    }

    fn grant_record(&self, grant: Option<&Value>, subject: &str) -> Option<SessionRecord> {
        let grant = grant.filter(|grant| grant.is_object())?;
        let subject = Value::String(subject.into());
        let subject = bounded(Some(&subject), 500)?;
        let token = grant
            .get("token")
            .and_then(Value::as_str)
            .filter(|token| is_app_token(token))?;
        let created_at = timestamp(grant.get("createdAt"))?;
        (grant.get("subject").and_then(Value::as_str) == Some(subject)
            && self.binding.matches(grant))
        .then(|| SessionRecord::new(subject, self.binding, created_at, token))
    }

    /// Store a fresh session for an exchanged grant. A successfully stored
    /// replacement atomically removes `previous`; any failure keeps it intact.
    pub async fn create(
        &self,
        grant: Option<&Value>,
        subject: &str,
        previous: Option<&str>,
    ) -> Result<String, GatewayError> {
        let record = self.grant_record(grant, subject).ok_or_else(unavailable)?;
        let token = format!("{LOCAL_TOKEN_PREFIX}{}", base64url(&self.random(32)?));
        let key = session_key(&token).map_err(|_| unavailable())?;
        let previous_key = previous
            .filter(|token| crate::binding::is_persistent_session(token))
            .map(session_key)
            .transpose()
            .map_err(|_| unavailable())?;
        let previous_value = match &previous_key {
            Some(key) => self.read(key).await?,
            None => None,
        };
        let revision = crate::binding::sha256_hex(&self.random(16)?);
        let mut mutations = vec![RecordMutation::Put {
            key: key.clone(),
            record: StoredRecord::json(revision, record.to_json()?, None),
        }];
        mutations.extend(previous_key.map(|key| RecordMutation::Delete { key }));
        let absent = RevisionCheck {
            key,
            expected_revision: None,
        };
        self.commit(vec![absent], mutations).await?;
        let replaced = previous_value
            .as_ref()
            .and_then(|value| SessionRecord::parse(value, self.binding));
        if let Some(replaced) = replaced {
            // The replaced local session is already irrevocably removed.
            let _ = self.upstream(&replaced, true).await;
        }
        Ok(token)
    }

    /// Introspect a session against Passport's current user. An authoritative
    /// 401 deletes local authority; anything else unavailable keeps it.
    pub async fn get(&self, token: &str) -> Result<Identity, GatewayError> {
        if !crate::binding::is_persistent_session(token) {
            return Err(unauthorized());
        }
        let key = session_key(token).map_err(|_| unavailable())?;
        let value = self.read(&key).await?.ok_or_else(unauthorized)?;
        let record = SessionRecord::parse(&value, self.binding).ok_or_else(unavailable)?;
        let reply = self
            .upstream(&record, false)
            .await
            .map_err(|_| unavailable())?;
        if reply.status == 401 {
            self.delete(key).await?;
            return Err(unauthorized());
        }
        if !(200..300).contains(&reply.status) || reply.body.len() > PASSPORT_SESSION_MAX_BYTES {
            return Err(unavailable());
        }
        let body: Value = serde_json::from_slice(&reply.body).map_err(|_| unavailable())?;
        let identity = identity(&body, &record, self.binding).ok_or_else(unavailable)?;
        // Logout owns deletion. An in-flight introspection can never republish it.
        let current = self.read(&key).await?;
        let current = current.and_then(|value| SessionRecord::parse(&value, self.binding));
        if current.is_none_or(|current| current.app_session_token != record.app_session_token) {
            return Err(unauthorized());
        }
        Ok(identity)
    }

    /// Local revocation completes before the independent upstream best-effort request.
    pub async fn revoke(&self, token: &str) -> Result<RevokeOutcome, GatewayError> {
        if !crate::binding::is_persistent_session(token) {
            return Ok(RevokeOutcome::NotApplicable);
        }
        let key = session_key(token).map_err(|_| unavailable())?;
        let value = self.read(&key).await?;
        self.delete(key).await?;
        let Some(record) = value.and_then(|value| SessionRecord::parse(&value, self.binding))
        else {
            return Ok(RevokeOutcome::NotApplicable);
        };
        Ok(match self.upstream(&record, true).await {
            Ok(reply) if reply.status == 204 => RevokeOutcome::Revoked,
            _ => RevokeOutcome::Unavailable,
        })
    }
}
