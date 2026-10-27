use gateway_auth_primitives::crypto::Secret;
use gateway_core::error::GatewayError;
#[derive(Clone, Debug)]
pub enum AuthUpstreamRequest {
    GitHubRepository {
        token: Secret,
        owner: String,
        repo: String,
    },
    DenoApplication {
        token: Secret,
        app: String,
    },
    DenoConsole {
        token: Secret,
        organization: String,
        app: String,
    },
    DenoDeployment {
        token: Secret,
        deployment: String,
    },
    PassportJwks,
    PassportPkceExchange {
        code: Secret,
        code_verifier: Secret,
    },
    PassportSession {
        app_session_token: Secret,
    },
    PassportRevoke {
        app_session_token: Secret,
    },
    PassportExchange {
        code: Secret,
        client_id: String,
        audience: String,
        origin: String,
        redirect_uri: String,
    },
}

impl AuthUpstreamRequest {
    pub fn url(&self) -> String {
        let component = |value: &str| {
            url::form_urlencoded::byte_serialize(value.as_bytes()).collect::<String>()
        };
        match self {
            Self::GitHubRepository { owner, repo, .. } => format!(
                "https://api.github.com/repos/{}/{}",
                component(owner),
                component(repo)
            ),
            Self::DenoApplication { app, .. } => {
                format!("https://api.deno.com/v2/apps/{}", component(app))
            }
            Self::DenoConsole {
                organization, app, ..
            } => format!(
                "https://console.deno.com/{}/{}",
                component(organization),
                component(app)
            ),
            Self::DenoDeployment { deployment, .. } => format!(
                "https://api.deno.com/v1/deployments/{}",
                component(deployment)
            ),
            Self::PassportPkceExchange { .. } => {
                gateway_auth_passport_contracts::passport::PASSPORT_EXCHANGE_URL.into()
            }
            Self::PassportSession { .. } => {
                gateway_auth_passport_contracts::session::PASSPORT_SESSION_URL.into()
            }
            Self::PassportRevoke { .. } => {
                gateway_auth_passport_contracts::session::PASSPORT_REVOKE_URL.into()
            }
            Self::PassportJwks => {
                gateway_auth_passport_contracts::passport::PASSPORT_JWKS_URL.into()
            }
            Self::PassportExchange { .. } => {
                gateway_auth_passport_contracts::passport::PASSPORT_EXCHANGE_URL.into()
            }
        }
    }

    pub fn method(&self) -> &'static str {
        match self {
            Self::PassportExchange { .. }
            | Self::PassportPkceExchange { .. }
            | Self::PassportSession { .. }
            | Self::PassportRevoke { .. } => "POST",
            _ => "GET",
        }
    }

    /// Fixed typed bodies only. A request-selected URL or body never exists.
    pub fn body(&self) -> Result<Vec<u8>, GatewayError> {
        match self {
            Self::PassportPkceExchange { code, code_verifier } => Ok(serde_json::to_vec(&serde_json::json!({
                "code":code.expose(), "codeVerifier":code_verifier.expose(), "appSession":true,
                "clientId":"ai", "audience":"ai", "origin":"https://ai.ubq.fi", "redirectUri":"https://ai.ubq.fi/auth/callback"
            })).map_err(|_| GatewayError::unavailable("Passport body encoding failed", "passport_unavailable"))?),
            Self::PassportSession { .. } | Self::PassportRevoke { .. } => Ok(br#"{"clientId":"ai","audience":"ai","origin":"https://ai.ubq.fi","redirectUri":"https://ai.ubq.fi/auth/callback"}"#.to_vec()),
            Self::PassportExchange {
                code,
                client_id,
                audience,
                origin,
                redirect_uri,
            } => serde_json::to_vec(&serde_json::json!({
                "code": code.expose(),
                "clientId": client_id,
                "audience": audience,
                "origin": origin,
                "redirectUri": redirect_uri,
            }))
            .map_err(|_| {
                GatewayError::unavailable(
                    "Passport exchange body could not be encoded.",
                    "passport_unavailable",
                )
            }),
            _ => Ok(Vec::new()),
        }
    }
    /// Source bounds apply only to the new app-session operations.
    pub fn app_session_operation(&self) -> bool {
        matches!(
            self,
            Self::PassportPkceExchange { .. }
                | Self::PassportSession { .. }
                | Self::PassportRevoke { .. }
        )
    }
    pub fn status_only(&self, status: u16) -> bool {
        matches!(self, Self::PassportRevoke { .. })
            || matches!(self, Self::PassportSession { .. }) && status == 401
    }
    pub fn headers(&self) -> Vec<(String, String)> {
        match self {
            Self::GitHubRepository { token, .. } => vec![
                ("authorization".into(), format!("Bearer {}", token.expose())),
                ("accept".into(), "application/vnd.github+json".into()),
                ("x-github-api-version".into(), "2022-11-28".into()),
                ("user-agent".into(), "ai.ubq.fi".into()),
            ],
            Self::DenoConsole { token, .. } => vec![
                (
                    "cookie".into(),
                    format!("token={}; deno_auth_ghid=force", token.expose()),
                ),
                ("accept".into(), "text/html".into()),
            ],
            Self::DenoApplication { token, .. } | Self::DenoDeployment { token, .. } => vec![
                ("authorization".into(), format!("Bearer {}", token.expose())),
                ("accept".into(), "application/json".into()),
            ],
            Self::PassportJwks => vec![("accept".into(), "application/json".into())],
            Self::PassportSession { app_session_token }
            | Self::PassportRevoke { app_session_token } => vec![
                (
                    "authorization".into(),
                    format!("Bearer {}", app_session_token.expose()),
                ),
                ("accept".into(), "application/json".into()),
                ("content-type".into(), "application/json".into()),
            ],
            Self::PassportExchange { .. } | Self::PassportPkceExchange { .. } => vec![
                ("accept".into(), "application/json".into()),
                ("content-type".into(), "application/json".into()),
            ],
        }
    }
}

#[derive(Clone, Debug)]
pub struct AuthUpstreamResponse {
    pub status: u16,
    pub body: Vec<u8>,
}

impl AuthUpstreamResponse {
    pub fn success(&self) -> bool {
        (200..300).contains(&self.status)
    }
}
