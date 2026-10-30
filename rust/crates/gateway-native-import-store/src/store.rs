use crate::durability;
use gateway_auth_native_contracts::native_import::NativeImportMetadata;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_authority::native_control::import::effects::NativeImportClaim;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority_contracts::{NativeRenewalCancellation, TokenSource};
use gateway_authority_native_records::import_effects::NativeImportBinding;
use gateway_core::state::AuthorityStore;
use gateway_source::error::GatewayError;
use std::{fs::File, path::Path};
/// Explicit registry construction, never an uploaded home or environment default.
pub struct ExistingAccountImportStore {
    pub(crate) directory: File,
    pub(crate) home: std::path::PathBuf,
    pub(crate) native_owner_id: String,
    pub(crate) account_id_hash: String,
}
impl ExistingAccountImportStore {
    pub fn configured(
        registered: &gateway_authority_native_records::owner::RegisteredRenewalOwner,
        account_id_hash: String,
    ) -> Result<Self, GatewayError> {
        let native_owner_id = registered.native_owner_id().to_owned();
        let home = Path::new(registered.registered_home());
        if native_owner_id.is_empty()
            || !gateway_auth_credentials::provider_credentials::valid_native_generation(
                &account_id_hash,
            )
        {
            return Err(unavailable());
        }
        Ok(Self {
            directory: durability::open_home(home)?,
            home: home.into(),
            native_owner_id,
            account_id_hash,
        })
    }
    /// Verify trusted bootstrap DATA against actual kernel custody; this does not grant mutation.
    pub fn prepare_exclusive_owner(
        &self,
        registered: &gateway_authority_native_records::owner::RegisteredRenewalOwner,
        writer_uid: u32,
        native_client_uids: &[u32],
        inspector: &Path,
        inspector_sha256: &str,
        deadline: std::time::Instant,
    ) -> Result<gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner, GatewayError>
    {
        gateway_native_custody_engine::exclusive_owner::ExclusiveManagedOwner::prepare(
            (
                &self.directory,
                &self.home,
                &self.native_owner_id,
                &self.account_id_hash,
            ),
            registered,
            writer_uid,
            native_client_uids,
            inspector,
            inspector_sha256,
            deadline,
        )
    }
    /// CF's generation marker does not exclude the independent native daemon.
    /// No advisory lock or caller assertion is accepted as that missing ownership proof.
    pub fn require_mutation_owner(&self) -> Result<(), GatewayError> {
        Err(GatewayError::unavailable(
            "Native import cannot replace credentials without proven exclusive native writer ownership.",
            "native_import_owner_exclusion_unavailable",
        ))
    }
    pub(crate) fn require_binding(
        &self,
        binding: &NativeImportBinding,
    ) -> Result<(), GatewayError> {
        if binding.native_owner_id != self.native_owner_id
            || binding.account_id_hash != self.account_id_hash
        {
            return Err(unavailable());
        }
        Ok(())
    }
    /// Read-only restart reconciliation. It never writes, refreshes, starts, or marks Published.
    /// A caller-created NativeGeneration or receipt JSON cannot enter this API as proof.
    pub async fn reconcile<S: AuthorityStore, I: TokenSource, C: NativeRenewalCancellation>(
        &self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
        claim: &NativeImportClaim,
        proof: &AuthenticatedNativeImport,
        original_body: &[u8],
    ) -> Result<durability::DurableNativeImport, GatewayError> {
        self.require_binding(claim.binding())?;
        if proof.metadata().action_ref != claim.binding().action_ref
            || proof.metadata().operation_id != claim.binding().operation_id
        {
            return Err(unavailable());
        }
        authority.recheck_import(proof).await?;
        authority.recheck_import_claim(claim).await?;
        proof.require_body(original_body, authority.now_ms()?)?;
        let receipt = durability::reconcile(self, claim.binding(), &sha256_hex(original_body))?;
        authority.recheck_import_claim(claim).await?;
        authority.recheck_import(proof).await?;
        proof.require_body(original_body, authority.now_ms()?)?;
        durability::recheck(self, &receipt)?;
        Ok(receipt)
    }
    #[cfg(feature = "native-import-fixture")]
    pub fn issue_fixture(
        &self,
        start: gateway_authority::native_control::import::effects::NativeImportStart<'_>,
        proof: &AuthenticatedNativeImport,
        original_body: &[u8],
        context: &gateway_native_http_admission::native_import::NativeImportContext<'_>,
        original: &gateway_auth_native_proof::native_context::NativeControlContext<'_>,
        now_ms: i64,
    ) -> Result<durability::DurableNativeImport, GatewayError> {
        let inputs = gateway_native_import_context::context::NativeImportInputs::checked(
            proof,
            proof.metadata(),
            original_body,
            context,
            original,
        )?;
        durability::issue_fixture(self, start, &inputs, now_ms)
    }
    /// A recovered proof still requires an exact current native reread before publication.
    pub async fn recheck_receipt<
        S: AuthorityStore,
        I: TokenSource,
        C: NativeRenewalCancellation,
    >(
        &self,
        authority: &NativeRenewalAuthority<'_, S, I, C>,
        claim: &NativeImportClaim,
        proof: &AuthenticatedNativeImport,
        original_body: &[u8],
        receipt: &durability::DurableNativeImport,
    ) -> Result<(), GatewayError> {
        self.require_binding(receipt.binding())?;
        let current = self
            .reconcile(authority, claim, proof, original_body)
            .await?;
        if current.binding() != receipt.binding() || current.receipt_ref() != receipt.receipt_ref()
        {
            return Err(unavailable());
        }
        durability::recheck(self, receipt)
    }
}
fn unavailable() -> GatewayError {
    GatewayError::unavailable(
        "Native import durability is absent, changed, or mismatched.",
        "native_import_durability_unavailable",
    )
}

impl ExistingAccountImportStore {
    pub(crate) fn live_root(&self) -> gateway_native_import_root::NativeRootView<'_> {
        gateway_native_import_root::NativeRootView::borrow_registered_store(
            &self.directory,
            &self.home,
            &self.native_owner_id,
            &self.account_id_hash,
        )
    }
    pub fn refresh_once<'a>(
        &self,
        validation: &mut gateway_native_import_validation::validation::ImportValidation,
        pending: gateway_native_import_validation::validation::NeedsRefresh401<'a>,
        custody: &mut gateway_native_import_request::live::LiveImportCustody<'_>,
        context: &gateway_native_import_context::context::NativeImportInputs<'_>,
    ) -> Result<gateway_native_import_validation::validation::ValidatedNativeImport<'a>, GatewayError>
    {
        let store =
            gateway_native_import_validation::journal::RotationStore::borrow_registered_store(
                &self.directory,
                &self.home,
                &self.native_owner_id,
                &self.account_id_hash,
            );
        validation.refresh_once(pending, custody, &store, context)
    }
}
