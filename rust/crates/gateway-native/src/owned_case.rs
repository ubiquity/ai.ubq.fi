//! Shared actual-child harness for the confined Linux test modules.
pub(crate) type TestResult<T = ()> = Result<T, Box<dyn std::error::Error>>;
/// Fixture threads report failures as text because a boxed error cannot cross threads.
pub(crate) type ThreadResult<T = ()> = Result<T, String>;
/// Converts a missing value or a failed result into the test's error instead of a panic.
pub(crate) trait Must<T> {
    fn must(self) -> TestResult<T>;
}
impl<T> Must<T> for Option<T> {
    fn must(self) -> TestResult<T> {
        self.ok_or_else(|| "missing value".into())
    }
}
impl<T, E: std::fmt::Debug> Must<T> for Result<T, E> {
    fn must(self) -> TestResult<T> {
        self.map_err(|error| format!("{error:?}").into())
    }
}
pub(crate) fn text(error: impl std::fmt::Debug) -> String {
    format!("{error:?}")
}
// A seccomp TSYNC policy is process-wide: these actual cases must not share
// the parallel library-test process. Each child remains inside the runner's
// existing namespace and applies the original native policy exactly once.
struct OwnedCaseChild(Option<std::process::Child>);
impl Drop for OwnedCaseChild {
    fn drop(&mut self) {
        if let Some(mut child) = self.0.take() {
            if child.try_wait().ok().flatten().is_none() {
                let _ = child.kill();
            }
            let _ = child.wait();
        }
    }
}
pub(crate) fn run_owned_case(selector: &str, input: &[u8], marker: &str) -> TestResult {
    use std::io::{Read, Write};
    use std::process::{Command, Stdio};
    let mut owned = OwnedCaseChild(Some(
        Command::new(std::env::current_exe()?)
            .args([
                "--ignored",
                "--exact",
                selector,
                "--nocapture",
                "--test-threads=1",
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?,
    ));
    let child = owned.0.as_mut().must()?;
    child.stdin.take().must()?.write_all(input)?;
    let stdout = child.stdout.take().must()?;
    let stderr = child.stderr.take().must()?;
    let drain = |stream: Box<dyn Read + Send>| {
        std::thread::spawn(move || {
            let mut bytes = Vec::new();
            stream
                .take(1_048_577)
                .read_to_end(&mut bytes)
                .map(|_| bytes)
        })
    };
    let out = drain(Box::new(stdout));
    let err = drain(Box::new(stderr));
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    let status = loop {
        if let Some(status) = child.try_wait()? {
            break status;
        }
        assert!(
            std::time::Instant::now() < deadline,
            "owned native fixture exceeded twenty seconds"
        );
        std::thread::sleep(std::time::Duration::from_millis(5));
    };
    owned.0.take();
    let stdout = out.join().must()??;
    let stderr = err.join().must()??;
    assert!(
        stdout.len() <= 1_048_576 && stderr.len() <= 1_048_576,
        "bounded child diagnostics exceeded"
    );
    assert!(
        status.success(),
        "actual isolated native case failed: {} {}",
        String::from_utf8_lossy(&stdout),
        String::from_utf8_lossy(&stderr)
    );
    assert!(
        String::from_utf8_lossy(&stdout).contains(marker),
        "real child acceptance marker missing"
    );
    Ok(())
}
pub(crate) fn observe_kernel_contract() {
    use landlock::{
        ABI, Access, AccessFs, AccessNet, CompatLevel, Compatible, Ruleset, RulesetAttr, Scope,
    };
    let kernel = std::fs::read_to_string("/proc/sys/kernel/osrelease")
        .map(|s| s.trim().to_owned())
        .unwrap_or_else(|error| format!("unreadable:{:?}", error.kind()));
    let construction: Result<(), String> = (|| {
        let rules = Ruleset::default()
            .set_compatibility(CompatLevel::HardRequirement)
            .handle_access(AccessFs::from_all(ABI::V9))
            .map_err(|error| format!("fs:{error:?}"))?
            .handle_access(AccessNet::from_all(ABI::V9))
            .map_err(|error| format!("net:{error:?}"))?
            .scope(Scope::AbstractUnixSocket | Scope::Signal)
            .map_err(|error| format!("scope:{error:?}"))?
            .create()
            .map_err(|error| format!("create:{error:?}"))?;
        drop(rules);
        Ok(())
    })();
    // Creating/dropping this observation ruleset never restricts any thread.
    // It is diagnostic only: the original required enforce() remains below,
    // with no ABI downgrade, capability-dependent success or skipped assertion.
    eprintln!("LINUX_NATIVE_KERNEL kernel={kernel} required=ABI_V9 construction={construction:?}");
}
