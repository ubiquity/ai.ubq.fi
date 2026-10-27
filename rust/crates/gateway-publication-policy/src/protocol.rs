use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_TIME_HEADER,
    NATIVE_VERSION_HEADER,
};
use gateway_source::error::GatewayError;
use std::future::Future;

#[derive(Clone)]
pub struct OwnedNativeAuthHeaders {
    version: String,
    key_id: String,
    time_ms: String,
    nonce: String,
    mac: String,
}
impl OwnedNativeAuthHeaders {
    pub fn from_pairs(
        pairs: impl IntoIterator<Item = (String, String)>,
    ) -> Result<Self, GatewayError> {
        let pairs: Vec<_> = pairs.into_iter().collect();
        let one = |name: &str| -> Result<String, GatewayError> {
            let values: Vec<_> = pairs
                .iter()
                .filter(|(key, _)| key.eq_ignore_ascii_case(name))
                .collect();
            if values.len() != 1 || values[0].1.len() > 256 {
                return Err(invalid());
            }
            Ok(values[0].1.clone())
        };
        Ok(Self {
            version: one(NATIVE_VERSION_HEADER)?,
            key_id: one(NATIVE_KEY_HEADER)?,
            time_ms: one(NATIVE_TIME_HEADER)?,
            nonce: one(NATIVE_NONCE_HEADER)?,
            mac: one(NATIVE_MAC_HEADER)?,
        })
    }
    pub fn pairs(&self) -> Vec<(String, String)> {
        vec![
            (NATIVE_VERSION_HEADER.into(), self.version.clone()),
            (NATIVE_KEY_HEADER.into(), self.key_id.clone()),
            (NATIVE_TIME_HEADER.into(), self.time_ms.clone()),
            (NATIVE_NONCE_HEADER.into(), self.nonce.clone()),
            (NATIVE_MAC_HEADER.into(), self.mac.clone()),
        ]
    }
}

pub struct PublicationRequest {
    pub method: String,
    pub path: String,
    pub raw_body: Vec<u8>,
    pub headers: OwnedNativeAuthHeaders,
}
pub struct PublicationResponse {
    pub status: u16,
    pub headers: Vec<(String, String)>,
    pub raw_body: Vec<u8>,
}

pub fn error_response(error: GatewayError) -> PublicationResponse {
    let raw_body = match serde_json::to_vec(&error.envelope) {
        Ok(raw_body) => raw_body,
        Err(_) => panic!("String-only typed error encoding"),
    };
    PublicationResponse {
        status: error.status,
        headers: vec![
            ("content-type".into(), "application/json".into()),
            ("cache-control".into(), "no-store".into()),
        ],
        raw_body,
    }
}

/// The native adapter owns one exact constructor-selected Worker/TLS target.
/// No URL is supplied to this operation and no alternate target is attempted.
pub trait PublicationCarrier {
    fn send(
        &self,
        request: PublicationRequest,
    ) -> impl Future<Output = Result<PublicationResponse, GatewayError>>;
}

pub fn unverified() -> GatewayError {
    GatewayError::unavailable(
        "Native publication response is not an authenticated matching receipt",
        "native_publication_response_unverified",
    )
}

pub fn decode_response_error(status: u16, body: &[u8]) -> Result<GatewayError, GatewayError> {
    let value: serde_json::Value = serde_json::from_slice(body).map_err(|_| unverified())?;
    let error = value
        .get("error")
        .and_then(serde_json::Value::as_object)
        .ok_or_else(unverified)?;
    let message = error
        .get("message")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(unverified)?;
    let kind = error
        .get("type")
        .and_then(serde_json::Value::as_str)
        .ok_or_else(unverified)?;
    let code = error.get("code").and_then(serde_json::Value::as_str);
    let mut result = GatewayError::new(status, message, code).with_type(kind);
    if let Some(param) = error.get("param") {
        if !param.is_null() && !param.is_string() {
            return Err(unverified());
        }
        result = result.with_param(param.as_str());
    }
    Ok(result)
}

pub fn valid_revision(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 256
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'_' | b'-' | b'.'))
}

pub fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Invalid fixed native publication payload",
        Some("native_publication_invalid"),
    )
}
