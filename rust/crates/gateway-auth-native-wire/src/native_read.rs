//! Fixed native read DATA and matching descriptors, without MAC or transport ownership.
use crate::validation::invalid;
use gateway_source::error::GatewayError;
use serde::{Deserialize, Serialize};

pub const NATIVE_WIRE_VERSION: u8 = 1;
pub const NATIVE_RPC_PATH: &str = "/v1/native/rpc";
pub const NATIVE_FOLLOW_PATH: &str = "/v1/native/follow";
pub const NATIVE_CONTROL_PATH: &str = "/v1/native/control";
pub const NATIVE_RENEWAL_AUTHORITY_PATH: &str = "/uos/native/renewal-authority";
pub const NATIVE_PUBLICATION_PATH: &str = "/uos/native/publication";
pub const NATIVE_RESPONSE_MAC_HEADER: &str = "x-uos-native-response-mac";
pub const NATIVE_MAX_BODY_BYTES: usize = 1_048_576;
pub const NATIVE_AUTH_WINDOW_MS: i64 = 30_000;
pub const NATIVE_NONCE_CAP: usize = 4096;
pub const NATIVE_VERSION_HEADER: &str = "x-uos-native-version";
pub const NATIVE_KEY_HEADER: &str = "x-uos-native-key-id";
pub const NATIVE_TIME_HEADER: &str = "x-uos-native-time-ms";
pub const NATIVE_NONCE_HEADER: &str = "x-uos-native-nonce";
pub const NATIVE_MAC_HEADER: &str = "x-uos-native-mac";

#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum NativeReadRequest {
    SupervisorSessions,
    SupervisorOutput {
        source_id: String,
        thread_id: String,
        cursor: Option<String>,
    },
    SupervisorBrief {
        source_id: String,
        thread_id: String,
    },
}

impl<'de> Deserialize<'de> for NativeReadRequest {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        // Serde internally tagged unit variants ignore extra fields even with
        // deny_unknown_fields. A zero-field struct preserves the public unit
        // variant while enforcing the exact finite wire object.
        #[derive(Deserialize)]
        #[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
        enum Wire {
            #[serde(rename = "supervisor_sessions")]
            Sessions {},
            #[serde(rename = "supervisor_output")]
            Output {
                source_id: String,
                thread_id: String,
                cursor: Option<String>,
            },
            #[serde(rename = "supervisor_brief")]
            Brief {
                source_id: String,
                thread_id: String,
            },
        }
        Ok(match Wire::deserialize(deserializer)? {
            Wire::Sessions {} => Self::SupervisorSessions,
            Wire::Output {
                source_id,
                thread_id,
                cursor,
            } => Self::SupervisorOutput {
                source_id,
                thread_id,
                cursor,
            },
            Wire::Brief {
                source_id,
                thread_id,
            } => Self::SupervisorBrief {
                source_id,
                thread_id,
            },
        })
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeRpcEnvelope {
    pub v: u8,
    pub request: NativeReadRequest,
}

impl NativeRpcEnvelope {
    pub fn validate(&self) -> Result<(), GatewayError> {
        if self.v != NATIVE_WIRE_VERSION {
            return Err(invalid("Unsupported native protocol version"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum AuthStream {
    /// Native inventory validates this selection when minted and again on open.
    /// Only the native carrier may resolve its source; it contains no URL/path.
    SupervisorFollow {
        source_id: String,
        thread_id: String,
        cursor: Option<String>,
    },
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct NativeRpcResponse {
    pub v: u8,
    pub status: u16,
    pub body: serde_json::Value,
    pub stream: Option<AuthStream>,
}

impl NativeRpcResponse {
    pub fn validate_for(&self, envelope: &NativeRpcEnvelope) -> Result<(), GatewayError> {
        envelope.validate()?;
        let valid = match (&envelope.request, &self.stream) {
            (
                NativeReadRequest::SupervisorOutput {
                    source_id,
                    thread_id,
                    cursor,
                },
                Some(AuthStream::SupervisorFollow {
                    source_id: actual_source,
                    thread_id: actual_thread,
                    cursor: actual_cursor,
                }),
            ) => {
                self.status == 200
                    && source_id == actual_source
                    && thread_id == actual_thread
                    && cursor == actual_cursor
            }
            (NativeReadRequest::SupervisorOutput { .. }, None) => self.status >= 400,
            (_, None) => true,
            _ => false,
        };
        if self.v != NATIVE_WIRE_VERSION || !(200..=599).contains(&self.status) || !valid {
            return Err(GatewayError::unavailable(
                "Native protocol response is invalid",
                "native_protocol_invalid",
            ));
        }
        Ok(())
    }
}
