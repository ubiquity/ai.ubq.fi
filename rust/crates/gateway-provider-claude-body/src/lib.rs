//! Defining provider claude-body operations; no transport or authority I/O.
// Optional grammar rejection is distinct from a propagated construction error.
macro_rules! represented { ($value:expr) => { match $value { Some(value) => value, None => return Ok(None) } }; }


pub mod finalize;

pub mod source_anthropic;

pub mod source_controls;

pub mod source_request;

pub mod types;
