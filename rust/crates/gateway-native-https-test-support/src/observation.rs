//! Plain observed request data from a synthetic TLS socket.
use std::collections::BTreeMap;
pub struct Observed {
    pub sni: String,
    pub line: String,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}
