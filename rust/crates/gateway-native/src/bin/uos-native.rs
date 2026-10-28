#[cfg(unix)]
use gateway_source::error::GatewayError;
#[cfg(unix)]
fn main() {
    if let Err(error) = run() {
        eprintln!(
            "[uos-native] {}",
            error
                .envelope
                .error
                .code
                .as_deref()
                .unwrap_or("native_startup_failed")
        );
        std::process::exit(1);
    }
}
#[cfg(not(unix))]
fn main() {
    eprintln!("[uos-native] native_confinement_unavailable");
    std::process::exit(1);
}

#[cfg(unix)]
fn run() -> Result<(), GatewayError> {
    gateway_native_connector::artifact::require_production_artifact()?;
    use gateway_native::daemon_config::NativeDaemonConfig;
    use gateway_native_confinement::confinement::{NativeConfinement, enforce};
    use gateway_native_http_admission::http_server::NativeHttpService;
    use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
    use gateway_native_supervisor::supervisor::SourceId;
    use gateway_native_supervisor_data::data::{SupervisorData, SupervisorSourceConfig};
    use std::{net::TcpListener, path::PathBuf};
    let config = NativeDaemonConfig::read_stdin(std::io::stdin().lock())?;
    let replay_directory = config
        .nonce_database
        .parent()
        .ok_or_else(|| nonce_store_unavailable("Replay database has no parent"))?
        .to_owned();
    let database_name = config
        .nonce_database
        .file_name()
        .and_then(|name| name.to_str())
        .ok_or_else(|| nonce_store_unavailable("Replay database name is invalid"))?;
    let allowed = [
        database_name.to_owned(),
        format!("{database_name}-wal"),
        format!("{database_name}-shm"),
    ];
    for entry in std::fs::read_dir(&replay_directory)
        .map_err(|_| nonce_store_unavailable("Private replay directory is unavailable"))?
    {
        let entry = entry
            .map_err(|_| nonce_store_unavailable("Private replay directory is unavailable"))?;
        if !allowed
            .iter()
            .any(|name| entry.file_name() == std::ffi::OsStr::new(name))
        {
            return Err(nonce_store_unavailable(
                "Replay storage must have a dedicated directory",
            ));
        }
    }
    let listener = TcpListener::bind(config.listen).map_err(|_| {
        GatewayError::unavailable(
            "Native listener could not bind",
            "native_listener_unavailable",
        )
    })?;
    let address = listener.local_addr().map_err(|_| {
        GatewayError::unavailable(
            "Native listener has no address",
            "native_listener_unavailable",
        )
    })?;
    let nonces = SqliteNonceStore::open(&config.nonce_database)?;
    let mut sources = Vec::new();
    let mut readonly_paths = Vec::new();
    let mut unix_sockets = Vec::new();
    let mut list_directories = Vec::new();
    for source in config.sources {
        let socket = source.socket_path().to_owned();
        unix_sockets.push(socket.clone());
        if let Some(home) = source.codex_home().filter(|home| home.is_dir()) {
            prepare_source_paths(home, &mut readonly_paths, &mut list_directories)?;
        }
        sources.push(SupervisorSourceConfig::configured(
            SourceId::parse(source.id())?,
            source.name().into(),
            socket,
            source.codex_home().map(PathBuf::from),
        )?);
    }
    let hostname = std::fs::read_to_string("/proc/sys/kernel/hostname")
        .ok()
        .map(|name| name.trim().to_owned())
        .filter(|name| !name.is_empty());
    let data = SupervisorData::configured(sources)?.with_metadata(hostname, Vec::new());
    let renewal = config
        .owner_renewal
        .map(|renewal| {
            prepare_owner_renewal(
                renewal,
                &config.identity,
                &mut readonly_paths,
                &mut unix_sockets,
            )
        })
        .transpose()?;
    enforce(&NativeConfinement {
        readonly_paths,
        list_directories,
        replay_directory,
        unix_sockets,
    })?;
    println!(
        "{}",
        serde_json::json!({"event":"native_listener_ready","v":1,"address":address.to_string()})
    );
    let service = NativeHttpService::configured(config.identity, nonces, data);
    let service = match renewal {
        Some(renewal) => service.with_owner_renewal(renewal),
        None => service,
    };
    service.serve(listener)
}

#[cfg(unix)]
fn nonce_store_unavailable(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "native_nonce_store_unavailable")
}

#[cfg(unix)]
fn prepare_source_paths(
    home: &std::path::Path,
    readonly_paths: &mut Vec<std::path::PathBuf>,
    list_directories: &mut Vec<std::path::PathBuf>,
) -> Result<(), GatewayError> {
    use std::path::PathBuf;
    let canonical_home = std::fs::canonicalize(home)
        .map_err(|_| source_unavailable("Configured source home is unavailable"))?;
    list_directories.push(canonical_home.clone());
    for entry in std::fs::read_dir(home)
        .map_err(|_| source_unavailable("Configured source directory is unavailable"))?
    {
        let entry =
            entry.map_err(|_| source_unavailable("Configured source entry is unavailable"))?;
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if matches!(name.as_ref(), "sessions" | "archived_sessions")
            || name
                .strip_prefix("state_")
                .and_then(|name| name.strip_suffix(".sqlite"))
                .is_some_and(|number| {
                    !number.is_empty() && number.bytes().all(|byte| byte.is_ascii_digit())
                })
        {
            let canonical = std::fs::canonicalize(entry.path())
                .map_err(|_| source_unavailable("Configured source path is unavailable"))?;
            if !canonical.starts_with(&canonical_home) {
                return Err(source_unavailable(
                    "Configured source path escapes its home",
                ));
            }
            readonly_paths.push(canonical);
            for suffix in ["-wal", "-shm"] {
                let companion = PathBuf::from(format!("{}{suffix}", entry.path().display()));
                if companion.is_file() {
                    readonly_paths.push(companion);
                }
            }
        }
    }
    Ok(())
}

#[cfg(unix)]
fn source_unavailable(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "native_source_unavailable")
}

#[cfg(unix)]
fn prepare_owner_renewal(
    config: gateway_native::daemon_config::OwnerRenewalConfig,
    identity: &gateway_auth_native_proof::native_auth::NativeTransportIdentity,
    readonly_paths: &mut Vec<std::path::PathBuf>,
    unix_sockets: &mut Vec<std::path::PathBuf>,
) -> Result<
    gateway_native::owner_renewal::NativeOwnerRenewalService<
        gateway_native::managed_publication_http::NativeAccessPublicationHttp,
    >,
    GatewayError,
> {
    use gateway_auth_credentials::native_credentials::NativeOwnerId;
    use gateway_auth_native_wire::native_read::{
        NATIVE_PUBLICATION_PATH, NATIVE_RENEWAL_AUTHORITY_PATH,
    };
    use gateway_native::{
        authority_http::NativeAuthorityHttp, managed_publication_http::NativeAccessPublicationHttp,
        owner_renewal::NativeOwnerRenewalService,
    };
    use gateway_native_local_transport::unix::NativeOwnerLocation;
    let mut locations = Vec::new();
    for owner in config.owners() {
        let home = std::fs::canonicalize(owner.codex_home()).map_err(|_| renewal_unavailable())?;
        if !home.is_dir() {
            return Err(renewal_unavailable());
        }
        let auth =
            std::fs::canonicalize(home.join("auth.json")).map_err(|_| renewal_unavailable())?;
        let socket = std::fs::canonicalize(home.join("app-server-control/app-server-control.sock"))
            .map_err(|_| renewal_unavailable())?;
        if !auth.starts_with(&home) || !socket.starts_with(&home) || !auth.is_file() {
            return Err(renewal_unavailable());
        }
        readonly_paths.push(auth);
        unix_sockets.push(socket);
        locations.push(NativeOwnerLocation::configured(
            NativeOwnerId(owner.owner_id().into()),
            home,
        )?);
    }
    let origin = url::Url::parse(config.authority_origin()).map_err(|_| renewal_unavailable())?;
    let authority = origin
        .join(NATIVE_RENEWAL_AUTHORITY_PATH)
        .map_err(|_| renewal_unavailable())?;
    let publication = origin
        .join(NATIVE_PUBLICATION_PATH)
        .map_err(|_| renewal_unavailable())?;
    // Both transports are prepared before OS confinement. Each connector owns
    // exactly its existing FD and cannot use DNS, redirects or a replacement FD.
    let authority_socket = preconnect(&authority)?;
    let publication_socket = preconnect(&publication)?;
    NativeOwnerRenewalService::configured(
        NativeAuthorityHttp::from_connected(
            authority.as_str(),
            authority_socket,
            identity.clone(),
        )?,
        NativeAccessPublicationHttp::from_connected(
            publication.as_str(),
            publication_socket,
            identity.clone(),
        )?,
        locations,
    )
}

#[cfg(unix)]
fn preconnect(target: &url::Url) -> Result<std::net::TcpStream, GatewayError> {
    use std::{net::ToSocketAddrs, time::Duration};
    let host = target.host_str().ok_or_else(renewal_unavailable)?;
    let port = target
        .port_or_known_default()
        .ok_or_else(renewal_unavailable)?;
    for address in (host, port)
        .to_socket_addrs()
        .map_err(|_| renewal_unavailable())?
    {
        if let Ok(socket) = std::net::TcpStream::connect_timeout(&address, Duration::from_secs(10))
        {
            return Ok(socket);
        }
    }
    Err(renewal_unavailable())
}

#[cfg(unix)]
fn renewal_unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Configured native owner renewal is unavailable",
        "native_owner_renewal_unavailable",
    )
}
