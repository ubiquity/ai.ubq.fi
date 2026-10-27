//! Serialized custody DATA only; a decoded frame never grants stop, mutation or completion.
use gateway_authority::native_control::import::custody_session::ImportCustodyStartedRequest;
use gateway_core::error::GatewayError;
use serde::{Deserialize, Serialize};

// Five 4096-byte authority-text fields may each escape to six bytes per UTF8 byte.
// Two original ASCII references use 256 bytes, three hashes 64 and safe time 16 digits.
// Exact serializer maximum is asserted against the complete required shape below.
pub const STARTED_BINDING_MAX: usize = 123_831;
pub const CUSTODY_FRAME_MAX: usize = 148_689;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Header {
    pub v: u8,
    pub sequence: u64,
    pub session_ref: String,
    pub challenge: String,
    pub binding: ImportCustodyStartedRequest,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Release {
    pub v: u8,
    pub sequence: u64,
    pub session_ref: String,
    pub challenge: String,
    pub binding: ImportCustodyStartedRequest,
    pub publication_revision: String,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Stopped {
    pub v: u8,
    pub sequence: u64,
    pub session_ref: String,
    pub challenge: String,
    pub binding: ImportCustodyStartedRequest,
    pub epoch: String,
    pub root_device: u64,
    pub root_inode: u64,
}
#[derive(Serialize, Deserialize)]
#[serde(tag = "operation", rename_all = "snake_case")]
pub enum Frame {
    Quiesce(Header),
    CheckStopped(Header),
    ReleaseAfterVerifiedTerminal(Release),
    Cancel(Header),
    Stopped(Stopped),
    Refused(Header),
    Unknown(Header),
    Released(Header),
}
pub struct HeaderView<'a> {
    pub sequence: u64,
    pub session_ref: &'a str,
    pub challenge: &'a str,
    pub binding: &'a ImportCustodyStartedRequest,
}
impl Frame {
    pub fn header(&self) -> HeaderView<'_> {
        match self {
            Self::Quiesce(h)
            | Self::CheckStopped(h)
            | Self::Cancel(h)
            | Self::Refused(h)
            | Self::Unknown(h)
            | Self::Released(h) => HeaderView {
                sequence: h.sequence,
                session_ref: &h.session_ref,
                challenge: &h.challenge,
                binding: &h.binding,
            },
            Self::ReleaseAfterVerifiedTerminal(h) => HeaderView {
                sequence: h.sequence,
                session_ref: &h.session_ref,
                challenge: &h.challenge,
                binding: &h.binding,
            },
            Self::Stopped(h) => HeaderView {
                sequence: h.sequence,
                session_ref: &h.session_ref,
                challenge: &h.challenge,
                binding: &h.binding,
            },
        }
    }
    fn version(&self) -> u8 {
        match self {
            Self::Quiesce(h)
            | Self::CheckStopped(h)
            | Self::Cancel(h)
            | Self::Refused(h)
            | Self::Unknown(h)
            | Self::Released(h) => h.v,
            Self::ReleaseAfterVerifiedTerminal(h) => h.v,
            Self::Stopped(h) => h.v,
        }
    }
    pub fn validate(&self) -> Result<(), GatewayError> {
        let h = self.header();
        h.binding.validate()?;
        if self.version() != 1
            || h.sequence == 0
            || !hash(h.session_ref)
            || !hash(h.challenge)
            || h.binding.canonical_bytes()?.len() > STARTED_BINDING_MAX
        {
            return Err(unavailable());
        }
        match self {
            Self::ReleaseAfterVerifiedTerminal(h)
                if h.publication_revision.trim().is_empty()
                    || h.publication_revision.len() > 4096 =>
            {
                Err(unavailable())
            }
            Self::Stopped(h) if !hash(&h.epoch) || h.root_inode == 0 => Err(unavailable()),
            _ => Ok(()),
        }
    }
    pub fn encode(&self) -> Result<Vec<u8>, GatewayError> {
        self.validate()?;
        let bytes = serde_json::to_vec(self).map_err(|_| unavailable())?;
        if bytes.len() > CUSTODY_FRAME_MAX {
            return Err(unavailable());
        }
        Ok(bytes)
    }
    pub fn decode(bytes: &[u8]) -> Result<Self, GatewayError> {
        if bytes.is_empty() || bytes.len() > CUSTODY_FRAME_MAX {
            return Err(unavailable());
        }
        let frame: Self = serde_json::from_slice(bytes).map_err(|_| unavailable())?;
        frame.validate()?;
        Ok(frame)
    }
}
impl HeaderView<'_> {
    pub fn copy_data(&self) -> Result<Header, GatewayError> {
        Ok(Header {
            v: 1,
            sequence: self.sequence,
            session_ref: self.session_ref.into(),
            challenge: self.challenge.into(),
            binding: copy_binding(self.binding)?,
        })
    }
}
pub fn copy_binding(
    request: &ImportCustodyStartedRequest,
) -> Result<ImportCustodyStartedRequest, GatewayError> {
    serde_json::from_slice(&request.canonical_bytes()?).map_err(|_| unavailable())
}
pub fn hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import custody is unavailable; reconcile the original action",
        "native_import_custody_unknown",
    )
}
