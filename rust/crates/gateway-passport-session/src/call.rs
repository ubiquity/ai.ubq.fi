//! The two fixed upstream session requests. Neither URL nor body is request-selected.
use crate::binding::Binding;
use gateway_core::error::GatewayError;
use std::future::Future;

pub const PASSPORT_SESSION_URL: &str = "https://passport.ubq.fi/api/auth/sso/session";
pub const PASSPORT_SESSION_REVOKE_URL: &str = "https://passport.ubq.fi/api/auth/sso/session/revoke";
/// Source `readJson` bound for an introspection reply.
pub const PASSPORT_SESSION_MAX_BYTES: usize = 65_536;

/// One introspection or revocation of an app grant. The grant is a bearer
/// secret, so `Debug` never prints it.
#[derive(Clone)]
pub struct SessionCall {
    app_session_token: String,
    binding: Binding,
    revoke: bool,
}

impl std::fmt::Debug for SessionCall {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("SessionCall")
            .field("app_session_token", &"[redacted]")
            .field("binding", &self.binding)
            .field("revoke", &self.revoke)
            .finish()
    }
}

impl SessionCall {
    pub fn new(app_session_token: String, binding: Binding, revoke: bool) -> Self {
        Self {
            app_session_token,
            binding,
            revoke,
        }
    }

    pub fn is_revoke(&self) -> bool {
        self.revoke
    }

    pub fn url(&self) -> &'static str {
        if self.revoke {
            PASSPORT_SESSION_REVOKE_URL
        } else {
            PASSPORT_SESSION_URL
        }
    }

    pub fn method(&self) -> &'static str {
        "POST"
    }

    pub fn headers(&self) -> Vec<(String, String)> {
        vec![
            (
                "authorization".into(),
                format!("Bearer {}", self.app_session_token),
            ),
            ("content-type".into(), "application/json".into()),
            ("accept".into(), "application/json".into()),
        ]
    }

    pub fn body(&self) -> Result<Vec<u8>, GatewayError> {
        serde_json::to_vec(&self.binding.to_json()).map_err(|_| {
            GatewayError::unavailable(
                "Passport session body could not be encoded.",
                "passport_unavailable",
            )
        })
    }
}

#[derive(Clone, Debug)]
pub struct SessionReply {
    pub status: u16,
    pub body: Vec<u8>,
}

/// Host capabilities. Implementations must disable automatic redirects, as the
/// source `redirect: "error"` does, and bound the request time.
pub trait SessionHost {
    fn random_bytes(&self, length: usize) -> Result<Vec<u8>, GatewayError>;
    fn fetch_session(
        &self,
        call: SessionCall,
    ) -> impl Future<Output = Result<SessionReply, GatewayError>>;
}
