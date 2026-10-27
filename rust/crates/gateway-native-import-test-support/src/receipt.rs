//! Synthetic temporary-home and document DATA only; no live writer/custody/receipt grant.
use crate::fixture::{Fixture, fail, now_ms};
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
    pub fn fresh() -> Result<Self, gateway_core::error::GatewayError> {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        let root = fs::canonicalize(std::env::temp_dir()).map_err(|_| fail())?;
        assert!(root.is_dir());
        let path = root.join(format!(
            "native-import-receipt-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&path).map_err(|_| fail())?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).map_err(|_| fail())?;
        Ok(Self(path))
    }
    pub fn write_auth(
        &self,
        value: &serde_json::Value,
    ) -> Result<(), gateway_core::error::GatewayError> {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(self.0.join("auth.json"))
            .map_err(|_| fail())?;
        file.write_all(&serde_json::to_vec(value).map_err(|_| fail())?)
            .map_err(|_| fail())?;
        file.sync_all().map_err(|_| fail())?;
        Ok(())
    }
}
impl Drop for Home {
    fn drop(&mut self) {
        // Drop cannot propagate; cleanup failure must still fail the fixture.
        if let Err(error) = fs::remove_dir_all(&self.0) {
            panic!("Synthetic home cleanup failed: {error}");
        }
    }
}
pub fn document(label: &str) -> Result<serde_json::Value, gateway_core::error::GatewayError> {
    let access = format!(
        "fixture.{}.signature",
        base64url(
            serde_json::to_string(&json!({"exp":now_ms()?/1000+3600,"label":label}))
                .map_err(|_| fail())?
                .as_bytes()
        )
    );
    Ok(
        json!({"tokens":{"account_id":"synthetic-account","access_token":access,"refresh_token":format!("SYNTHETIC_REFRESH_{label}")},"OPENAI_API_KEY":"SYNTHETIC_INDEPENDENT_KEY","other":{"independent":label}}),
    )
}
pub fn fixture(
    home: &Home,
    old: &serde_json::Value,
) -> Result<Fixture, gateway_core::error::GatewayError> {
    let mut f = Fixture::new()?;
    let credentials =
        gateway_auth::native_credentials::parse_native_document(old).map_err(|_| fail())?;
    let key = codex_source_pool_key();
    let mut source = {
        let rows = f.store.rows.lock().map_err(|_| fail())?;
        gateway_auth::provider_credentials::parse_codex_source_pool(
            &rows.get(&key.0).ok_or_else(fail)?.value,
        )
        .map_err(|_| fail())?
        .ok_or_else(fail)?
    };
    let native = source.accounts[0].native_owner.as_mut().ok_or_else(fail)?;
    native.codex_home = home.0.to_str().ok_or_else(fail)?.into();
    native
        .managed_source
        .as_mut()
        .ok_or_else(fail)?
        .generation_hash = credentials.generation_hash();
    f.store
        .seed(key, "source-1", source.private_source_value())?;
    f.owner = RegisteredRenewalOwner::configured(
        "native-owner".into(),
        home.0.to_str().ok_or_else(fail)?.into(),
    )
    .map_err(|_| fail())?;
    Ok(f)
}
