//! Complete fixed native authority header and response DATA adaptation.
use crate::cancellation::Cancellation;
use crate::headers::{HeaderLimits, bounded_headers};
use gateway_auth_native_contracts::transport::NativeAuthHeaders;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_RESPONSE_MAC_HEADER,
    NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
};
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_source::error::GatewayError;
use worker::{Request, Response};

pub struct Headers {
    version: String,
    key_id: String,
    time_ms: String,
    pub nonce: String,
    mac: String,
}
impl Headers {
    pub fn read(request: &Request) -> Result<Self, GatewayError> {
        let values = bounded_headers(
            request,
            HeaderLimits {
                total_bytes: 16_384,
                value_bytes: 256,
                fields: 2000,
            },
        )?;
        let one = |name: &str| {
            let entries = values
                .iter()
                .filter(|(key, _)| key.eq_ignore_ascii_case(name))
                .collect::<Vec<_>>();
            if entries.len() != 1 {
                Err(invalid())
            } else {
                Ok(entries[0].1.clone())
            }
        };
        Ok(Self {
            version: one(NATIVE_VERSION_HEADER)?,
            key_id: one(NATIVE_KEY_HEADER)?,
            time_ms: one(NATIVE_TIME_HEADER)?,
            nonce: one(NATIVE_NONCE_HEADER)?,
            mac: one(NATIVE_MAC_HEADER)?,
        })
    }
    pub fn borrowed(&self) -> NativeAuthHeaders<'_> {
        NativeAuthHeaders {
            version: &self.version,
            key_id: &self.key_id,
            time_ms: &self.time_ms,
            nonce: &self.nonce,
            mac: &self.mac,
        }
    }
}

pub struct Incoming(pub std::rc::Rc<Cancellation>);
impl NativeRenewalCancellation for Incoming {
    fn cancelled(&self) -> bool {
        self.0.signal().aborted()
    }
}

pub fn invalid() -> GatewayError {
    GatewayError::new(
        400,
        "Native authority request is invalid",
        Some("native_authority_invalid"),
    )
}

pub fn response(status: u16, body: Vec<u8>, mac: String) -> Result<Response, GatewayError> {
    let mut response = Response::from_bytes(body)
        .map_err(|_| invalid())?
        .with_status(status);
    for (key, value) in [
        ("Content-Type", "application/json"),
        ("Cache-Control", "no-store"),
        (NATIVE_RESPONSE_MAC_HEADER, &mac),
    ] {
        response
            .headers_mut()
            .set(key, value)
            .map_err(|_| invalid())?;
    }
    Ok(response)
}
