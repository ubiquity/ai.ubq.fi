//! First-class OpenRouter Decisions route (`POST /v1/decisions`, cfe8458).
//! Decision models answer typed questions about `state`; this crate validates the
//! documented envelope, builds the one fixed wire request and maps the upstream.
//! The Worker owns transport and the single original extras admission.
pub mod handler;
pub mod host;
pub mod request;
