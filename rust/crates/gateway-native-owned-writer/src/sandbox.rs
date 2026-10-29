use crate::{fail, pin::PinnedExecutable};
use gateway_source::error::GatewayError;
use std::path::Path;
const SANDBOX_SHA256S: [&str; 3] = [
    "e3d7a792c58a5d3783d2f7274c82d70062393830d8cb1ded713ca554a470bd2f",
    "abc5bb136d6b5cce8fa85d789f78e3326c51ca60cae637b2064adfb67a1dcd9a",
    "58839ef01b4eef8aac0d2aa8f9d1c074ae45aafe3533965b030672450064acc8",
];
pub(crate) fn verified_tool(path: &Path, ds: &[&str]) -> Result<PinnedExecutable, GatewayError> {
    ds.iter()
        .find_map(|digest| PinnedExecutable::verify(path, digest).ok())
        .ok_or_else(fail)
}
pub(crate) fn sandbox_executable() -> Result<PinnedExecutable, GatewayError> {
    let binary = verified_tool(Path::new("/usr/bin/sandbox-exec"), &SANDBOX_SHA256S)?;
    (binary.facts.uid == 0).then_some(binary).ok_or_else(fail)
}
