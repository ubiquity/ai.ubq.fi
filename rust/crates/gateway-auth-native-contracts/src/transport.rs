//! Ordinary native capability selectors and borrowed transport header DATA.
#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum NativeCapability {
    SupervisorRead,
    ConfigurationPublish,
    AccessPublish,
    OwnerRenewal,
    CodexAuthImport,
    NativeProfileImport,
    LiveSideband,
}

#[derive(Clone, Copy)]
pub struct NativeAuthHeaders<'a> {
    pub version: &'a str,
    pub key_id: &'a str,
    pub time_ms: &'a str,
    pub nonce: &'a str,
    pub mac: &'a str,
}

#[derive(Clone, Copy)]
pub struct NativeImportAuthHeaders<'a> {
    pub auth: NativeAuthHeaders<'a>,
    pub body_sha256: &'a str,
}
