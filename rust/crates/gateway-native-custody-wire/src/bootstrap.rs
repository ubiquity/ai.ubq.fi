//! Closed serialized native bootstrap/endpoint DATA; no shape or address proves a parent, peer, role or custody.
use crate::wire::{self as data, unavailable};
use gateway_core::error::GatewayError;
use serde::{Deserialize, Serialize};
use std::net::SocketAddr;

/// DATA from the actual parent's sole bootstrap pipe, never an HTTP option.
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ImportEndpointData {
    pub authority: SocketAddr,
    pub models: SocketAddr,
    pub oauth: SocketAddr,
}
impl ImportEndpointData {
    pub fn validate(&self) -> Result<(), GatewayError> {
        for address in self.addresses() {
            if address.port() == 0 || address.ip().is_unspecified() || address.ip().is_multicast() {
                return Err(unavailable());
            }
        }
        Ok(())
    }
    pub fn addresses(&self) -> [SocketAddr; 3] {
        [self.authority, self.models, self.oauth]
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Bootstrap {
    pub v: u8,
    pub session_ref: String,
    pub client_local: SocketAddr,
    pub client_peer: SocketAddr,
    pub custody_local: SocketAddr,
    pub custody_peer: SocketAddr,
    pub endpoints: ImportEndpointData,
    pub native_owner_id: String,
    pub account_id_hash: String,
}
impl Bootstrap {
    pub fn validate(&self) -> Result<(), GatewayError> {
        if self.native_owner_id.trim().is_empty()
            || self.native_owner_id.len() > 4096
            || !data::hash(&self.account_id_hash)
            || self.v != 1
            || !data::hash(&self.session_ref)
            || [
                self.client_local,
                self.client_peer,
                self.custody_local,
                self.custody_peer,
            ]
            .iter()
            .any(|a| !a.ip().is_loopback() || a.port() == 0)
            || self.custody_local == self.custody_peer
        {
            return Err(unavailable());
        }
        self.endpoints.validate()
    }
}
