//! Closed declaration DATA; the native wire selects only its three forwarding fields.
use crate::{payload, summary};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SummaryDeclaration {
    pub version: &'static str,
    pub prefix: &'static str,
    pub prefix_bytes: usize,
    pub prefix_sha256: &'static str,
    pub max_per_request: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ForwardedPayloadDeclaration {
    pub version: &'static str,
    pub mode: &'static str,
    pub per_message_limit: usize,
    pub summary_exemption: SummaryDeclaration,
}

pub const FORWARDED_PAYLOAD_DECLARATION: ForwardedPayloadDeclaration =
    ForwardedPayloadDeclaration {
        version: payload::VERSION,
        mode: "bytes",
        per_message_limit: payload::PER_MESSAGE_LIMIT,
        summary_exemption: SummaryDeclaration {
            version: summary::VERSION,
            prefix: summary::PREFIX,
            prefix_bytes: summary::PREFIX_BYTES,
            prefix_sha256: summary::PREFIX_SHA256,
            max_per_request: 1,
        },
    };
