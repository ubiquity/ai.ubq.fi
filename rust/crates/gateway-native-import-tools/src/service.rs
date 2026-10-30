#[cfg(feature = "native-import-fixture")]
use crate::fixed_fixture::PreparedCustodianFixtureService;
use crate::{
    endpoints::{CustodyAuthorityConnection, ResolvedImportEndpoints},
    launch::VerifiedImportLaunch,
};
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_daemon_config::config::NativeDaemonConfig;
use gateway_source::error::GatewayError;
use std::path::{Path, PathBuf};

pub struct ProductionCustodianService {
    actor: PathBuf,
    digest: String,
    endpoints: ResolvedImportEndpoints,
}
pub enum FixedCustodianService {
    Production(ProductionCustodianService),
    #[cfg(feature = "native-import-fixture")]
    Fixture(PreparedCustodianFixtureService),
}
impl FixedCustodianService {
    pub fn prepare_production(config: &[u8]) -> Result<Self, GatewayError> {
        gateway_native_connector::artifact::require_production_artifact()?;
        let prepared = NativeDaemonConfig::read_import_stdin(config)?;
        let endpoints = ResolvedImportEndpoints::resolve_import(&prepared)?;
        let actor = std::env::current_exe()
            .map_err(|_| unavailable())?
            .parent()
            .ok_or_else(unavailable)?
            .join("uos-native-import");
        let digest = gateway_auth_primitives::crypto::sha256_hex(
            &std::fs::read(&actor).map_err(|_| unavailable())?,
        );
        Ok(Self::Production(ProductionCustodianService {
            actor,
            digest,
            endpoints,
        }))
    }
    pub fn connect_custody_authority(
        &self,
        config: &NativeDaemonConfig,
    ) -> Result<CustodyAuthorityConnection, GatewayError> {
        match self {
            Self::Production(service) => service.endpoints.connect_custody_authority(config),
            #[cfg(feature = "native-import-fixture")]
            Self::Fixture(service) => service.connect_custody_authority(config),
        }
    }
    pub fn prepare_launch<'cfg>(
        &self,
        config: &'cfg [u8],
        root: &Path,
        replay: &Path,
        registered: (&str, &str),
    ) -> Result<VerifiedImportLaunch<'cfg>, GatewayError> {
        match self {
            Self::Production(service) => VerifiedImportLaunch::prepare(
                (&service.actor, &service.digest),
                config,
                root,
                replay,
                &service.endpoints,
                registered,
            ),
            #[cfg(feature = "native-import-fixture")]
            Self::Fixture(service) => service.prepare_launch(config, root, replay, registered),
        }
    }
}
