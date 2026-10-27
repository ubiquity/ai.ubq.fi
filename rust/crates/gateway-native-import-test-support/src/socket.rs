//! Real synthetic loopback request DATA and durable nonce storage, without a receive/proof factory.
use crate::fixture::{Fixture, Store, Tokens, fail, now_ms, run};
use gateway_auth::{crypto::base64url, native_import::NativeImportMetadata};
use gateway_authority::store::Authority;
use gateway_native_import_client::client::NativeImportClient;
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use std::{
    collections::BTreeMap,
    io::Write,
    net::{TcpListener, TcpStream},
    os::unix::fs::PermissionsExt,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
};

pub struct Directory(pub PathBuf);
fn fresh_directory() -> Result<Directory, gateway_core::error::GatewayError> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let path = std::fs::canonicalize(std::env::temp_dir())
        .map_err(|_| fail())?
        .join(format!(
            "native-context-http-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::SeqCst)
        ));
    std::fs::create_dir(&path).map_err(|_| fail())?;
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o700)).map_err(|_| fail())?;
    Ok(Directory(path))
}
impl Drop for Directory {
    fn drop(&mut self) {
        // Drop cannot propagate; cleanup failure must still fail the fixture.
        if let Err(error) = std::fs::remove_dir_all(&self.0) {
            panic!("Synthetic directory cleanup failed: {error}");
        }
    }
}

pub struct SocketRequest {
    pub accepted: TcpStream,
    pub client: TcpStream,
    pub nonces: SqliteNonceStore,
    pub metadata: NativeImportMetadata,
    pub headers: BTreeMap<String, String>,
    pub head: Vec<u8>,
    directory: Directory,
}
impl SocketRequest {
    pub fn prepared(
        f: &Fixture,
        authority: &Authority<Store, Tokens>,
        body: &[u8],
    ) -> Result<Self, gateway_core::error::GatewayError> {
        let directory = fresh_directory()?;
        let nonces =
            SqliteNonceStore::open(&directory.0.join("nonce.sqlite3")).map_err(|_| fail())?;
        let listener = TcpListener::bind("127.0.0.1:0").map_err(|_| fail())?;
        let endpoint = listener.local_addr().map_err(|_| fail())?;
        let actor = run(f.actor(authority))?;
        let client = NativeImportClient::configured(
            endpoint,
            authority,
            &f.identity,
            &f.owner,
            &f.config,
            &f.cancel,
        )
        .map_err(|_| fail())?;
        let prepared = run(client.prepare(&actor, f.target())).map_err(|_| fail())?;
        let nonce = base64url(&[13; 32]);
        let upload = client
            .request(prepared, body, now_ms()?, &nonce)
            .map_err(|_| fail())?;
        let mut head = format!("POST {} HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: {}\r\n", upload.path(), body.len()).into_bytes();
        for (name, value) in upload.headers() {
            head.extend_from_slice(format!("{name}: {value}\r\n").as_bytes());
        }
        head.extend_from_slice(b"\r\n");
        let metadata = upload.metadata().clone();
        *f.store.action_key.lock().map_err(|_| fail())? =
            Some(gateway_auth::store::key(serde_json::json!([
                "uos_ai",
                "authority",
                "native_owner_renewal",
                "v1",
                "action",
                metadata.action_ref
            ])));
        let client = TcpStream::connect(endpoint).map_err(|_| fail())?;
        let (accepted, _) = listener.accept().map_err(|_| fail())?;
        Ok(Self {
            accepted,
            client,
            nonces,
            metadata,
            headers: upload.headers().clone(),
            head,
            directory,
        })
    }
    pub fn send_head(&mut self) -> Result<(), gateway_core::error::GatewayError> {
        self.client.write_all(&self.head).map_err(|_| fail())
    }
    pub fn send(&mut self, body: &[u8]) -> Result<(), gateway_core::error::GatewayError> {
        self.send_head()?;
        self.client.write_all(body).map_err(|_| fail())
    }
    pub fn directory(&self) -> &std::path::Path {
        &self.directory.0
    }
}
