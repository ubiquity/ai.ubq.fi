//! Actual metadata DATA for an already-owned File; no snapshot grants pinning, ownership or custody.
use gateway_core::error::GatewayError;
use std::{fs::File, os::unix::fs::MetadataExt};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Facts {
    pub device: u64,
    pub inode: u64,
    pub uid: u32,
    pub gid: u32,
    pub mode: u32,
    pub links: u64,
    pub size: u64,
    pub ctime: i64,
    pub ctime_nsec: i64,
}
impl Facts {
    pub fn same_object(&self, other: &Self) -> bool {
        self.device == other.device
            && self.inode == other.inode
            && self.uid == other.uid
            && self.gid == other.gid
            && self.mode == other.mode
    }
    pub fn read(file: &File) -> Result<Self, GatewayError> {
        let m = file.metadata().map_err(|_| fail())?;
        Ok(Self {
            device: m.dev(),
            inode: m.ino(),
            uid: m.uid(),
            gid: m.gid(),
            mode: m.mode(),
            links: m.nlink(),
            size: m.size(),
            ctime: m.ctime(),
            ctime_nsec: m.ctime_nsec(),
        })
    }
}
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
