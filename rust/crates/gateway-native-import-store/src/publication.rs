//! Native receipt-bound sanitized command. Neither DATA nor HTTP200 is completion.
use crate::{
    durability::{self, DurableNativeImport},
    store::ExistingAccountImportStore,
};
use gateway_auth_credentials::provider_credentials::codex_credential_identity;
use gateway_auth_native_protocol::native_import::publication::NativeImportPublication;
use gateway_auth_native_wire::publication_command::{
    ImportAccessDocument, ImportCatalogData, ImportPublicationCommandData,
};
use gateway_auth_primitives::crypto::{Secret, sha256_hex};
use gateway_authority::native_control::import::{
    AuthenticatedNativeImport, remote::AuthenticatedImportTerminal,
};
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_import_context::context::{NativeImportInputs, RequestIdentity};
use gateway_native_import_publication_http::{
    AuthenticatedImportPublicationReply, ImportPublicationHttp,
};
use gateway_native_import_request::live::LiveImportCustody;
use gateway_native_import_source::catalog::CatalogValidation;
use gateway_native_import_validation::validation::ValidatedNativeImport;
use gateway_source::error::GatewayError;

/// Native-only producer; no JSON/receipt-DATA/Boolean constructor.
pub struct PreparedImportPublication<'a> {
    original: RequestIdentity<'a>,
    command: ImportPublicationCommandData,
    body: Vec<u8>,
}
impl<'a> PreparedImportPublication<'a> {
    pub fn from_durable(
        store: &ExistingAccountImportStore,
        custody: &mut LiveImportCustody<'_>,
        context: &NativeImportInputs<'a>,
        candidate: &ValidatedNativeImport<'_>,
        receipt: &DurableNativeImport,
    ) -> Result<Self, GatewayError> {
        let original = context.checked_request_identity()?;
        candidate.require_original_context(context)?;
        if receipt.binding() != &custody.copy_binding_data() {
            return Err(unavailable());
        }
        let projection = candidate.publication_projection(context, receipt.credentials())?;
        custody.check_live(&store.live_root(), context)?;
        durability::recheck(store, receipt)?;
        let binding = receipt.binding();
        let credentials = receipt.credentials();
        let access = codex_credential_identity(
            &credentials.account_id,
            &credentials.access_token,
            &Secret::new(String::new()),
        );
        let catalog = match &projection.catalog {
            CatalogValidation::Normalized {
                snapshot,
                client_version,
                upstream_status,
            } => ImportCatalogData::Normalized {
                snapshot: snapshot.clone(),
                client_version: client_version.clone(),
                upstream_status: *upstream_status,
            },
            CatalogValidation::AuthenticatedLimited => ImportCatalogData::AuthenticatedLimited {
                upstream_status: 429,
            },
        };
        let command = ImportPublicationCommandData {
            operation: "codex_auth_import_publish".into(),
            v: 1,
            action_ref: binding.action_ref.clone(),
            operation_id: binding.operation_id.clone(),
            claim_ref: binding.claim_ref.clone(),
            native_owner_id: binding.native_owner_id.clone(),
            owner_revision: binding.owner_revision.clone(),
            source_revision: binding.source_revision.clone(),
            normalized_revision: binding.normalized_revision.clone(),
            publication_revision: custody.publication_reference()?,
            account_id_hash: binding.account_id_hash.clone(),
            previous_native_source_generation: binding.source_native_generation.clone(),
            native_source_generation: receipt.native_generation().into(),
            access_generation: access.native_generation_hash,
            native_receipt_ref: receipt.receipt_ref(),
            document: ImportAccessDocument {
                account_id: credentials.account_id.clone(),
                access_token: credentials.access_token.expose().into(),
                updated_at_ms: projection.updated_at_ms,
                native_refreshed_at: credentials.native_refreshed_at.clone(),
            },
            catalog,
        };
        let body = command.bytes()?;
        custody.bind_publication(&sha256_hex(&body))?;
        custody.check_live(&store.live_root(), context)?;
        durability::recheck(store, receipt)?;
        Ok(Self {
            original,
            command,
            body,
        })
    }
    pub fn require_terminal(
        &self,
        terminal: &AuthenticatedImportTerminal,
        receipt: &DurableNativeImport,
        reply: Option<&AuthenticatedImportPublicationReply>,
    ) -> Result<(), GatewayError> {
        let current = terminal.terminal();
        let identity = codex_credential_identity(
            &receipt.credentials().account_id,
            &receipt.credentials().access_token,
            &Secret::new(String::new()),
        );
        if terminal.binding() != receipt.binding()
            || current.publication_revision != self.command.publication_revision
            || current.native_receipt_ref != self.command.native_receipt_ref
            || current.publication_request_digest != self.request_digest()
            || current.native_source_generation != receipt.native_generation()
            || current.access_generation != self.command.access_generation
            || current.credential_version != identity.credential_version.0
            || current.source_revision != self.command.publication_revision
            || current.normalized_revision != self.command.publication_revision
        {
            return Err(unavailable());
        }
        if let Some(reply) = reply {
            reply.require_current_terminal_data(current, &self.request_digest())?;
        }
        Ok(())
    }
    pub(crate) fn command(&self) -> &ImportPublicationCommandData {
        &self.command
    }
    pub(crate) fn body(&self) -> &[u8] {
        &self.body
    }
    pub(crate) fn request_digest(&self) -> String {
        sha256_hex(&self.body)
    }
}

impl PreparedImportPublication<'_> {
    pub fn publish(
        &self,
        sender: &ImportPublicationHttp,
        store: &ExistingAccountImportStore,
        custody: &mut LiveImportCustody<'_>,
        context: &NativeImportInputs<'_>,
        receipt: &DurableNativeImport,
    ) -> Result<AuthenticatedImportPublicationReply, GatewayError> {
        context.check_original()?;
        self.original.require_same(context)?;
        if receipt.binding() != &custody.copy_binding_data()
            || receipt.receipt_ref() != self.command.native_receipt_ref
        {
            return Err(unavailable());
        }
        custody.check_live(&store.live_root(), context)?;
        durability::recheck(store, receipt)?;
        if self.command.bytes()? != self.body() {
            return Err(unavailable());
        }
        let reply = sender.exchange_data(self.command(), context.deadline())?;
        context.check_original()?;
        durability::recheck(store, receipt)?;
        // Current authoritative terminal and original private claim are additionally
        // required by the final completion producer. This reply alone is not it.
        Ok(reply)
    }

    pub fn observe_terminal<C: gateway_authority_contracts::NativeRenewalCancellation>(
        &self,
        authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
        claim: &gateway_authority::native_control::import::effects::NativeImportClaim,
        receipt: &DurableNativeImport,
        reply: Option<&AuthenticatedImportPublicationReply>,
    ) -> Result<AuthenticatedImportTerminal, GatewayError> {
        let terminal = authority.observe_publication(
            claim,
            &self.command.publication_revision,
            &receipt.receipt_ref(),
            &self.request_digest(),
        )?;
        self.require_terminal(&terminal, receipt, reply)?;
        Ok(terminal)
    }
    pub fn completion_data(
        &self,
        terminal: &AuthenticatedImportTerminal,
        reply: Option<&AuthenticatedImportPublicationReply>,
    ) -> gateway_native_import_source::result::NativeImportCompletion {
        use gateway_auth_native_protocol::native_import::publication::NativeImportPublicationDisposition;
        use gateway_native_import_source::result::NativeImportCompletion;
        let disposition = reply
            .map(|r| r.disposition_data())
            .unwrap_or(NativeImportPublicationDisposition::AlreadyCurrent);
        match disposition {
            NativeImportPublicationDisposition::Applied => NativeImportCompletion::Published {
                publication_revision: terminal.terminal().publication_revision.clone(),
                native_receipt_ref: terminal.terminal().native_receipt_ref.clone(),
            },
            NativeImportPublicationDisposition::AlreadyCurrent => {
                NativeImportCompletion::AlreadyCurrent {
                    publication_revision: terminal.terminal().publication_revision.clone(),
                    native_receipt_ref: terminal.terminal().native_receipt_ref.clone(),
                }
            }
        }
    }
}
