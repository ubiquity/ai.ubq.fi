//! Fixed fixture role only. Recipe DATA does not establish native custody.
#![cfg(all(feature = "native-import-fixture", target_os = "macos"))]
use crate::endpoints::{CustodyAuthorityConnection, ResolvedImportEndpoints};
use crate::launch::VerifiedImportLaunch;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_native_daemon_config::config::PreparedNativeDaemonConfig;
use gateway_source::error::GatewayError;
use std::{
    fs,
    net::SocketAddr,
    os::unix::fs::MetadataExt,
    path::{Path, PathBuf},
};

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct ProcessSetup {
    home: PathBuf,
    config: serde_json::Value,
    authority: SocketAddr,
    models: SocketAddr,
    oauth: SocketAddr,
    account_id_hash: String,
}

pub struct PreparedCustodianFixture {
    pub home: PathBuf,
    pub config: serde_json::Value,
    pub account_id_hash: String,
    pub plan: FixedCustodianFixturePlan,
}

/// An unvalidated fixed recipe, never a checked endpoint/pin/custody proof.
pub struct FixedCustodianFixturePlan {
    authority: SocketAddr,
    models: SocketAddr,
    oauth: SocketAddr,
}

/// Contains the late fixed actor recipe and validated endpoint role.
/// Neither field is observable or replaceable outside this defining crate.
pub struct PreparedCustodianFixtureService {
    actor: PathBuf,
    actor_hash: String,
    endpoints: ResolvedImportEndpoints,
}

fn assert_owned_canonical_directory(path: &Path) {
    assert_eq!(fs::canonicalize(path).unwrap(), path);
    let metadata = fs::symlink_metadata(path).unwrap();
    assert!(metadata.is_dir() && !metadata.file_type().is_symlink());
    assert_eq!(metadata.uid(), rustix::process::getuid().as_raw());
}

fn assert_fixture_child_binding(base: &Path, root: &Path, home: &Path) {
    assert_owned_canonical_directory(base);
    assert_owned_canonical_directory(root);
    assert_eq!(root.parent(), Some(base));
    assert_eq!(home, root.join("home"));
    assert_owned_canonical_directory(home);
}

/// The sole original process.json read, before Store construction.
pub fn prepare_custodian() -> Result<PreparedCustodianFixture, GatewayError> {
    let root = std::env::current_dir().unwrap();
    let base = root.parent().unwrap();
    assert_fixture_child_binding(base, &root, &root.join("home"));
    let setup: ProcessSetup =
        serde_json::from_slice(&fs::read(root.join("process.json")).unwrap()).unwrap();
    assert_fixture_child_binding(&base, &root, &setup.home);
    Ok(PreparedCustodianFixture {
        home: setup.home,
        config: setup.config,
        account_id_hash: setup.account_id_hash,
        plan: FixedCustodianFixturePlan {
            authority: setup.authority,
            models: setup.models,
            oauth: setup.oauth,
        },
    })
}

impl FixedCustodianFixturePlan {
    /// Called only after the original inherited-listener accept/drop phase.
    /// Hash/path observations and endpoint validation must not move earlier.
    pub fn prepare_service(self) -> Result<PreparedCustodianFixtureService, GatewayError> {
        let actor = std::env::current_exe()
            .unwrap()
            .parent()
            .unwrap()
            .parent()
            .unwrap()
            .join("examples/uos-native-import-fixture");
        assert!(
            actor.is_file(),
            "build exact fixture artifact before controlled parent"
        );
        let actor = fs::canonicalize(actor).unwrap();
        let actor_hash = sha256_hex(&fs::read(&actor).unwrap());
        let endpoints =
            ResolvedImportEndpoints::fixture(self.authority, self.models, self.oauth).unwrap();
        Ok(PreparedCustodianFixtureService {
            actor,
            actor_hash,
            endpoints,
        })
    }
}

impl PreparedCustodianFixtureService {
    pub fn connect_custody_authority(
        &self,
        config: &PreparedNativeDaemonConfig,
    ) -> Result<CustodyAuthorityConnection, GatewayError> {
        self.endpoints.connect_custody_authority(config)
    }

    pub fn prepare_launch<'cfg>(
        &self,
        daemon_config: &'cfg [u8],
        owner_root: &Path,
        replay_directory: &Path,
        registered_owner: (&str, &str),
    ) -> Result<VerifiedImportLaunch<'cfg>, GatewayError> {
        VerifiedImportLaunch::prepare(
            (&self.actor, &self.actor_hash),
            daemon_config,
            owner_root,
            replay_directory,
            &self.endpoints,
            registered_owner,
        )
    }
}
