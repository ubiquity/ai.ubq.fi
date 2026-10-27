//! Synthetic temporary-home and document DATA only; no live writer/custody/receipt grant.
use crate::fixture::{Fixture, now_ms};
use gateway_auth::{crypto::base64url, provider_credentials::codex_source_pool_key};
use gateway_authority::native_control::receiver::RegisteredRenewalOwner;
use serde_json::json;
use std::{
    fs,
    io::Write,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};
pub struct Home(PathBuf);
impl Home {
    pub fn path(&self) -> &Path {
        &self.0
    }
    pub fn fresh() -> Self {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        let root = fs::canonicalize(std::env::temp_dir()).unwrap();
        assert!(root.is_dir());
        let path = root.join(format!(
            "native-import-receipt-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        Self(path)
    }
    pub fn write_auth(&self, value: &serde_json::Value) {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(self.0.join("auth.json"))
            .unwrap();
        file.write_all(&serde_json::to_vec(value).unwrap()).unwrap();
        file.sync_all().unwrap();
    }
}
impl Drop for Home {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
pub fn document(label: &str) -> serde_json::Value {
    let access = format!(
        "fixture.{}.signature",
        base64url(
            serde_json::to_string(&json!({"exp":now_ms()/1000+3600,"label":label}))
                .unwrap()
                .as_bytes()
        )
    );
    json!({"tokens":{"account_id":"synthetic-account","access_token":access,"refresh_token":format!("SYNTHETIC_REFRESH_{label}")},"OPENAI_API_KEY":"SYNTHETIC_INDEPENDENT_KEY","other":{"independent":label}})
}
pub fn fixture(home: &Home, old: &serde_json::Value) -> Fixture {
    let mut f = Fixture::new();
    let credentials = gateway_auth::native_credentials::parse_native_document(old).unwrap();
    let key = codex_source_pool_key();
    let mut source = {
        let rows = f.store.rows.lock().unwrap();
        gateway_auth::provider_credentials::parse_codex_source_pool(
            &rows.get(&key.0).unwrap().value,
        )
        .unwrap()
        .unwrap()
    };
    let native = source.accounts[0].native_owner.as_mut().unwrap();
    native.codex_home = home.0.to_str().unwrap().into();
    native.managed_source.as_mut().unwrap().generation_hash = credentials.generation_hash();
    f.store.seed(key, "source-1", source.private_source_value());
    f.owner =
        RegisteredRenewalOwner::configured("native-owner".into(), home.0.to_str().unwrap().into())
            .unwrap();
    f
}
