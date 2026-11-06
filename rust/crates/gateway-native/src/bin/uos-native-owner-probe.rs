//! One-shot same-FD passive owner metadata. Stdin is configuration, never an RPC or credential document.
#[cfg(target_os = "linux")]
fn main() {
    if let Err(error) = run() {
        eprintln!(
            "[uos-native-owner-probe] {}",
            error
                .envelope
                .error
                .code
                .as_deref()
                .unwrap_or("native_owner_probe_unavailable")
        );
        std::process::exit(1);
    }
}
#[cfg(not(target_os = "linux"))]
fn main() {
    std::process::exit(1);
}
#[cfg(target_os = "linux")]
fn run() -> Result<(), gateway_core::error::GatewayError> {
    use std::io::Read;
    #[derive(serde::Deserialize)]
    #[serde(deny_unknown_fields)]
    struct Input {
        v: u8,
        codex_home: std::path::PathBuf,
        expected_uid: u32,
    }
    let invalid = || {
        gateway_core::error::GatewayError::new(
            400,
            "Passive owner probe configuration is invalid",
            Some("native_owner_probe_configuration_invalid"),
        )
    };
    gateway_native::require_production_artifact()?;
    if std::env::args_os().nth(1).is_some() {
        return Err(invalid());
    }
    let mut bytes = Vec::new();
    std::io::stdin()
        .lock()
        .take(4097)
        .read_to_end(&mut bytes)
        .map_err(|_| invalid())?;
    if bytes.len() > 4096 {
        return Err(invalid());
    }
    let input: Input = serde_json::from_slice(&bytes).map_err(|_| invalid())?;
    if input.v != 1
        || input.expected_uid != rustix::process::geteuid().as_raw()
        || !input.codex_home.is_absolute()
    {
        return Err(invalid());
    }
    let home = std::fs::canonicalize(&input.codex_home).map_err(|_| invalid())?;
    if home.as_os_str() != input.codex_home.as_os_str() {
        return Err(invalid());
    }
    let socket = gateway_native_runtime::owner_socket::resolve(&home).map_err(|_| invalid())?;
    gateway_native::confinement::enforce_readonly_socket(&socket)?;
    let observation = gateway_native::unix::observe_owner(&home, &socket, input.expected_uid)?;
    serde_json::to_writer(std::io::stdout().lock(), &observation).map_err(|_| invalid())?;
    Ok(())
}
