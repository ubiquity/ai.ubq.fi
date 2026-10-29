use crate::bootstrap::{ActorBootstrap, InheritedCustodyChannel};
use crate::live::LiveImportCustody;
use gateway_auth_native_proof::{
    native_auth::NativeTransportIdentity,
    native_context::{NativeControlContext, NativeRequestLifetime},
};
use gateway_authority::native_control::import::{
    effects::NativeImportStart, remote::RemoteImportAuthority,
};
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_daemon_config::config::NativeDaemonConfig;
use gateway_native_http_framing::import_head::ImportHead;
use gateway_native_import_context::context::NativeImportInputs;
use gateway_native_import_context::document::{self, RemoteImportDocument};
use gateway_native_import_http::{AfterAuthority, AfterValidation};
use gateway_native_import_publication_http::ImportPublicationHttp;
use gateway_native_import_source::result::NativeImportResult;
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use gateway_source::error::GatewayError;
use std::{
    io::Read,
    net::{Shutdown, TcpStream},
    os::unix::fs::MetadataExt,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

pub struct StoreInputData {
    pub registered: RegisteredRenewalOwner,
    pub account_id_hash: String,
}
pub struct BeforeNativeStore {
    publication_config: NativeDaemonConfig,
    authority_config: NativeDaemonConfig,
    config: NativeDaemonConfig,
    boot: ActorBootstrap,
}
pub struct ActorCancellation {
    parent_dead: Arc<AtomicBool>,
    client: TcpStream,
    deadline: Instant,
}
impl NativeRenewalCancellation for ActorCancellation {
    fn cancelled(&self) -> bool {
        self.parent_dead.load(Ordering::Acquire)
            || Instant::now() >= self.deadline
            || gateway_native_http_framing::response::disconnected(&self.client)
    }
}
pub struct PreparedActor {
    nonces: Option<SqliteNonceStore>,
    tail: TransportTail,
}
pub struct TransportTail {
    publication_identity: Option<NativeTransportIdentity>,
    authority_identity: Option<NativeTransportIdentity>,
    identity: NativeTransportIdentity,
    origin: String,
    native_owner_id: String,
    account_id_hash: String,
    endpoints: Option<gateway_native_custody_wire::bootstrap::ImportEndpointData>,
    custody: Option<InheritedCustodyChannel>,
    client: TcpStream,
}
pub struct SetupFailure {
    error: GatewayError,
    custody_abort: Option<TcpStream>,
    client_abort: Option<TcpStream>,
    cancellation: Option<ActorCancellation>,
    nonces: Option<SqliteNonceStore>,
    before: BeforeNativeStore,
}
impl SetupFailure {
    pub fn release_before_store(self) -> (GatewayError, BeforeNativeStore) {
        drop(self.custody_abort);
        drop(self.client_abort);
        drop(self.cancellation);
        drop(self.nonces);
        (self.error, self.before)
    }
}
impl BeforeNativeStore {
    pub fn from_stdio() -> Result<(Self, StoreInputData), GatewayError> {
        let mut boot = ActorBootstrap::from_stdio()?;
        let config = NativeDaemonConfig::read_import_stdin(boot.config.as_slice())?;
        let authority_config = NativeDaemonConfig::read_import_stdin(boot.config.as_slice())?;
        let publication_config = NativeDaemonConfig::read_import_stdin(boot.config.as_slice())?;
        boot.config.fill(0);
        let renewal = config.owner_renewal.as_ref().ok_or_else(unavailable)?;
        let owner = renewal
            .owners()
            .iter()
            .find(|owner| owner.owner_id() == boot.native_owner_id)
            .ok_or_else(unavailable)?;
        let metadata = std::fs::symlink_metadata(owner.codex_home()).map_err(|_| unavailable())?;
        if !metadata.is_dir()
            || metadata.uid() != rustix::process::geteuid().as_raw()
            || metadata.mode() & 0o077 != 0
        {
            return Err(unavailable());
        }
        let registered = RegisteredRenewalOwner::configured(
            owner.owner_id().into(),
            owner.codex_home().to_str().ok_or_else(unavailable)?.into(),
        )?;
        let data = StoreInputData {
            registered,
            account_id_hash: boot.account_id_hash.clone(),
        };
        Ok((
            Self {
                publication_config,
                authority_config,
                config,
                boot,
            },
            data,
        ))
    }
    pub fn resume(self) -> Result<(PreparedActor, ActorCancellation, Instant), SetupFailure> {
        let mut failure = SetupFailure {
            error: unavailable(),
            custody_abort: None,
            client_abort: None,
            cancellation: None,
            nonces: None,
            before: self,
        };
        let nonces = match SqliteNonceStore::open(&failure.before.config.nonce_database) {
            Ok(value) => value,
            Err(error) => {
                failure.error = error;
                return Err(failure);
            }
        };
        failure.nonces = Some(nonces);
        let deadline = Instant::now() + Duration::from_secs(45);
        let parent_dead = Arc::new(AtomicBool::new(false));
        let client = match failure.before.boot.client.try_clone() {
            Ok(value) => value,
            Err(_) => return Err(failure),
        };
        failure.cancellation = Some(ActorCancellation {
            parent_dead: parent_dead.clone(),
            client,
            deadline,
        });
        failure.client_abort = match failure.before.boot.client.try_clone() {
            Ok(value) => Some(value),
            Err(_) => return Err(failure),
        };
        failure.custody_abort = match failure.before.boot.custody.socket.try_clone() {
            Ok(value) => Some(value),
            Err(_) => return Err(failure),
        };
        let client_abort = failure
            .client_abort
            .take()
            .expect("actual clone installed immediately above");
        let custody_abort = failure
            .custody_abort
            .take()
            .expect("actual clone installed immediately above");
        std::thread::spawn(move || {
            let mut unexpected = [0u8; 1];
            let _ = std::io::stdin().lock().read(&mut unexpected);
            parent_dead.store(true, Ordering::Release);
            let _ = client_abort.shutdown(Shutdown::Both);
            let _ = custody_abort.shutdown(Shutdown::Both);
        });
        let cancellation = failure
            .cancellation
            .take()
            .expect("actual cancellation installed before both clones");
        let nonces = failure.nonces.take();
        let BeforeNativeStore {
            publication_config,
            authority_config,
            config,
            boot,
        } = failure.before;
        let origin = config
            .owner_renewal
            .as_ref()
            .expect("same checked immutable config")
            .authority_origin()
            .into();
        let tail = TransportTail {
            publication_identity: Some(publication_config.identity),
            authority_identity: Some(authority_config.identity),
            identity: config.identity,
            origin,
            native_owner_id: boot.native_owner_id,
            account_id_hash: boot.account_id_hash,
            endpoints: Some(boot.endpoints),
            custody: Some(boot.custody),
            client: boot.client,
        };
        Ok((PreparedActor { nonces, tail }, cancellation, deadline))
    }
}
impl PreparedActor {
    pub fn release_nonces(mut self) -> TransportTail {
        self.nonces.take();
        self.tail
    }
    pub fn read_head(&mut self) -> Result<ImportHead, GatewayError> {
        let head = gateway_native_http_framing::request::read_import_head(&mut self.tail.client)?;
        gateway_native_http_admission::import_ingress::check_import_auth_headers(&head)?;
        Ok(head)
    }
    pub fn connect_authority<'a>(
        &mut self,
        cancellation: &'a ActorCancellation,
        deadline: Instant,
    ) -> Result<(RemoteImportAuthority<'a, ActorCancellation>, AfterAuthority), GatewayError> {
        let endpoints = self.tail.endpoints.take().ok_or_else(unavailable)?;
        let connections =
            gateway_native_import_http::connect_endpoints(endpoints, cancellation, deadline)?;
        connections.prepare_authority(
            &self.tail.origin,
            self.tail
                .authority_identity
                .take()
                .ok_or_else(unavailable)?,
            cancellation,
            deadline,
        )
    }
    pub fn read_original<'c>(
        &mut self,
        head: &ImportHead,
        authority: &RemoteImportAuthority<'_, ActorCancellation>,
        lifetime: NativeRequestLifetime<'c>,
    ) -> Result<(RemoteImportDocument, NativeControlContext<'c>), GatewayError> {
        document::read_remote_import(
            &mut self.tail.client,
            head,
            &self.tail.identity,
            self.nonces.as_ref().ok_or_else(unavailable)?,
            authority,
            lifetime,
            (&self.tail.native_owner_id, &self.tail.account_id_hash),
        )
    }
    pub fn prepare_publication(
        &mut self,
        remaining: AfterValidation,
    ) -> Result<ImportPublicationHttp, GatewayError> {
        remaining.prepare_publication(
            &self.tail.origin,
            self.tail
                .publication_identity
                .take()
                .ok_or_else(unavailable)?,
        )
    }
    pub fn begin_custody<'a>(
        &mut self,
        start: NativeImportStart<'a>,
        context: &NativeImportInputs<'_>,
    ) -> Result<LiveImportCustody<'a>, GatewayError> {
        LiveImportCustody::begin_live_custody(
            self.tail.custody.take().ok_or_else(unavailable)?,
            start,
            context,
        )
    }
    pub fn write_result(&mut self, result: NativeImportResult) -> Result<(), GatewayError> {
        gateway_native_http_framing::response::write_import_response(
            &mut self.tail.client,
            result.status,
            result.headers,
            result.body,
        )
    }
    pub fn write_error(&mut self, error: GatewayError) -> Result<(), GatewayError> {
        gateway_native_http_framing::response::write_error(&mut self.tail.client, error)
    }
}
