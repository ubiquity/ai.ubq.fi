mod pin;
mod sandbox;
use crate::pin::PinnedExecutable;
use gateway_source::error::GatewayError;
use std::{
    fs::File,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    time::{Duration, Instant},
};
fn fail() -> GatewayError {
    GatewayError::unavailable(
        "Exclusive native owner custody is unavailable or changed",
        "native_import_owner_exclusion_unavailable",
    )
}
pub struct UnlaunchedCodex(PinnedExecutable);
impl UnlaunchedCodex {
    pub fn verify(path: &Path) -> Result<Self, GatewayError> {
        Ok(Self(PinnedExecutable::verify(
            path,
            "8813367285524ac798dd96d1b071aea30d58708c7226fd236cd275cf1cfdee31",
        )?))
    }
}
pub struct FixedWriterPlan {
    sandbox: PinnedExecutable,
    profile: String,
    executable: PinnedExecutable,
    home: PathBuf,
}
pub struct ReadyWriter(FixedWriterPlan);
pub struct OwnedWriterChild {
    child: Option<Child>,
}
impl FixedWriterPlan {
    #[cfg(all(feature = "native-import-fixture", target_os = "macos"))]
    pub fn prepare_owned_fixture(home: &Path) -> Result<Self, GatewayError> {
        let executable = std::env::current_exe().map_err(|_| fail())?;
        let executable = std::fs::canonicalize(executable).map_err(|_| fail())?;
        let digest = gateway_auth_primitives::crypto::sha256_hex(
            &std::fs::read(&executable).map_err(|_| fail())?,
        );
        let executable = PinnedExecutable::verify(&executable, &digest)?;
        let quoted_home =
            serde_json::to_string(home.to_str().ok_or_else(fail)?).map_err(|_| fail())?;
        let path = serde_json::to_string(executable.path().to_str().ok_or_else(fail)?)
            .map_err(|_| fail())?;
        let profile = format!(
            "(version 1) (allow default) (deny network*) (deny file-write*) (allow file-write* (subpath {})) (deny process-fork) (deny process-exec*) (allow process-exec (literal {}))",
            quoted_home, path
        );
        let sandbox = sandbox::sandbox_executable()?;
        Ok(Self {
            sandbox,
            profile,
            executable,
            home: home.into(),
        })
    }
    pub fn recheck(self) -> Result<ReadyWriter, GatewayError> {
        self.executable.recheck()?;
        self.sandbox.recheck()?;
        Ok(ReadyWriter(self))
    }
}
impl ReadyWriter {
    pub fn spawn_fixed(self, errors: File) -> Result<OwnedWriterChild, GatewayError> {
        let plan = self.0;
        let child = Command::new(plan.sandbox.path())
            .args(["-p", plan.profile.as_str()])
            .arg(plan.executable.path())
            .args([
                "--exact",
                "existing_account_import::foundation_tests::controlled_async_writer_child",
                "--nocapture",
                "--ignored",
            ])
            .env_clear()
            .env("LC_ALL", "C")
            .env("LANG", "C")
            .env("TZ", "UTC")
            .env("UOS_NATIVE_OWNER_TEST_DIRECTORY", &plan.home)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::from(errors))
            .spawn()
            .map_err(|_| fail())?;
        Ok(OwnedWriterChild { child: Some(child) })
    }
}
impl OwnedWriterChild {
    pub fn stop_and_reap(&mut self, deadline: Instant) -> Result<(), GatewayError> {
        let child = self.child.as_mut().ok_or_else(fail)?;
        if child.try_wait().map_err(|_| fail())?.is_none() {
            child.kill().map_err(|_| fail())?;
        }
        loop {
            if child.try_wait().map_err(|_| fail())?.is_some() {
                break;
            }
            if Instant::now() >= deadline {
                return Err(fail());
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        self.child.take();
        Ok(())
    }
    pub fn abort_and_reap(&mut self) {
        if let Some(mut child) = self.child.take() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}
impl Drop for OwnedWriterChild {
    fn drop(&mut self) {
        self.abort_and_reap();
    }
}
