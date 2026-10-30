use crate::fail;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_native_confinement::file_observation::Facts;
use gateway_source::error::GatewayError;
use rustix::fs::{Mode, OFlags, openat};
use std::{
    fs::File,
    io::Read,
    path::{Path, PathBuf},
};

/// Hash plus descriptor metadata; no caller can turn a digest into a live kernel lease.
pub(crate) struct PinnedExecutable {
    file: File,
    path: PathBuf,
    pub facts: Facts,
    digest: String,
}
impl PinnedExecutable {
    pub(crate) fn verify(path: &Path, digest: &str) -> Result<Self, GatewayError> {
        if !path.is_absolute()
            || !gateway_auth_credentials::provider_credentials::valid_native_generation(digest)
        {
            return Err(fail());
        }
        let file = File::from(
            openat(
                rustix::fs::CWD,
                path,
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        let facts = Facts::read(&file)?;
        if facts.mode & 0o170000 != 0o100000
            || facts.mode & 0o022 != 0
            || facts.links != 1
            || facts.size > 300_000_000
        {
            return Err(fail());
        }
        // Seatbelt literals match resolved kernel paths, not the /tmp alias spelling.
        // The original final component was already opened NOFOLLOW; inode/hash must match again.
        let canonical = std::fs::canonicalize(path).map_err(|_| fail())?;
        let this = Self {
            file,
            path: canonical,
            facts,
            digest: digest.into(),
        };
        this.recheck()?;
        Ok(this)
    }
    pub(crate) fn path(&self) -> &Path {
        &self.path
    }
    pub(crate) fn recheck(&self) -> Result<(), GatewayError> {
        let reopened = File::from(
            openat(
                rustix::fs::CWD,
                &self.path,
                OFlags::RDONLY | OFlags::NOFOLLOW | OFlags::CLOEXEC | OFlags::NONBLOCK,
                Mode::empty(),
            )
            .map_err(|_| fail())?,
        );
        if Facts::read(&self.file)? != self.facts || Facts::read(&reopened)? != self.facts {
            return Err(fail());
        }
        let mut bytes = Vec::new();
        reopened
            .take(self.facts.size + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| fail())?;
        if bytes.len() as u64 != self.facts.size || sha256_hex(&bytes) != self.digest {
            return Err(fail());
        }
        Ok(())
    }
}
