//! Defining provider gateway-providers operations; no transport or authority I/O.
// Optional grammar rejection is distinct from a propagated construction error.
macro_rules! represented { ($value:expr) => { match $value { Some(value) => value, None => return Ok(None) } }; }


pub mod anthropic;

pub mod chat;

pub mod request;

pub mod source_request;
