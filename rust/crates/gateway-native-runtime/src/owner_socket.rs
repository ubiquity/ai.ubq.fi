//! Validate only the socket anchored by a configured home, including Codex's private relocated socket.
use std::{
    fs, io,
    os::unix::fs::{FileTypeExt, MetadataExt},
    path::{Path, PathBuf},
};

#[cfg(target_os = "linux")]
use std::os::unix::net::UnixStream;

fn denied() -> io::Error {
    io::Error::new(
        io::ErrorKind::PermissionDenied,
        "Native owner socket custody is unproved",
    )
}
fn private(metadata: &fs::Metadata) -> bool {
    metadata.uid() == rustix::process::geteuid().as_raw() && metadata.mode() & 0o077 == 0
}
pub fn resolve(home: &Path) -> io::Result<PathBuf> {
    let home = fs::canonicalize(home)?;
    if !private(&fs::metadata(&home)?) {
        return Err(denied());
    }
    let control = home.join("app-server-control");
    let directory = fs::symlink_metadata(&control)?;
    if !directory.is_dir() || !private(&directory) {
        return Err(denied());
    }
    let source = control.join("app-server-control.sock");
    let link = fs::symlink_metadata(&source)?;
    if link.uid() != rustix::process::geteuid().as_raw() {
        return Err(denied());
    }
    let socket = fs::canonicalize(&source)?;
    let metadata = fs::symlink_metadata(&socket)?;
    let parent = socket.parent().ok_or_else(denied)?;
    if !metadata.file_type().is_socket() || !private(&metadata) || !private(&fs::metadata(parent)?)
    {
        return Err(denied());
    }
    Ok(socket)
}
/// Recheck the actual connected peer; pathname custody alone never authenticates another UID.
#[cfg(target_os = "linux")]
pub fn verify_peer(stream: &UnixStream) -> io::Result<()> {
    if rustix::net::sockopt::socket_peercred(stream)?.uid != rustix::process::geteuid() {
        return Err(denied());
    }
    Ok(())
}
