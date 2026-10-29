//! Immutable build DATA supplied by the actual compiled Worker entrypoint.
#[derive(Clone, Copy)]
pub struct CaptureBuild {
    git_sha: &'static str,
    deployment_id: &'static str,
}
impl CaptureBuild {
    pub const fn new(git_sha: &'static str, deployment_id: &'static str) -> Self {
        Self {
            git_sha,
            deployment_id,
        }
    }
    pub fn git_sha(&self) -> &'static str {
        self.git_sha
    }
    pub fn deployment_id(&self) -> &'static str {
        self.deployment_id
    }
}
