use gateway_authority::native_control::import::{
    effects::NativeImportClaim,
    remote::{AuthenticatedImportTerminal, RemoteImportAuthority},
};
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_authority_native_records::import_effects::NativeImportMode;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_import_context::context::NativeImportInputs;
use gateway_native_import_publication_http::{
    AuthenticatedImportPublicationReply, ImportPublicationHttp,
};
use gateway_native_import_request::{live::LiveImportCustody, setup::PreparedActor};
use gateway_native_import_store::{
    durability::{self, DurableNativeImport},
    publication::PreparedImportPublication,
    store::ExistingAccountImportStore,
};
use gateway_native_import_validation::validation::{
    ImportValidation, ValidatedNativeImport, ValidationOutcome,
};
use gateway_source::error::GatewayError;
/// No caller-created document, Started DATA or receipt marker can construct it.
/// This is still not final publication/completion authority.
pub struct PreparedExistingImport<'a> {
    pub(crate) claim: NativeImportClaim,
    pub(crate) custody: LiveImportCustody<'a>,
    pub(crate) candidate: ValidatedNativeImport<'a>,
    pub(crate) receipt: DurableNativeImport,
    pub(crate) publication: PreparedImportPublication<'a>,
}
/// Genuine native durability, a fresh matching authoritative terminal and the
/// SAME live stopped peer's release exchange. No DATA/Boolean constructor.
pub struct VerifiedImportPublication<'a> {
    pub(crate) prepared: PreparedExistingImport<'a>,
    pub(crate) terminal: AuthenticatedImportTerminal,
    reply: Option<AuthenticatedImportPublicationReply>,
}
impl<'a> PreparedExistingImport<'a> {
    pub fn publish<C: NativeRenewalCancellation>(
        mut self,
        authority: &RemoteImportAuthority<'_, C>,
        sender: &ImportPublicationHttp,
        store: &ExistingAccountImportStore,
        context: &NativeImportInputs<'_>,
    ) -> Result<VerifiedImportPublication<'a>, GatewayError> {
        let result = (|| {
            authority.recheck_import_claim(&self.claim)?;
            let reply = self
                .publication
                .publish(sender, store, &mut self.custody, context, &self.receipt)
                .ok();
            // A lost/ambiguous publication reply may only reconcile the SAME
            // native receipt and command. Never rewrite, refresh or mint Start.
            let terminal = self.publication.observe_terminal(
                authority,
                &self.claim,
                &self.receipt,
                reply.as_ref(),
            )?;
            context.check_original()?;
            durability::recheck(store, &self.receipt)?;
            self.custody
                .release_after_verified_terminal(&terminal, context)?;
            context.check_original()?;
            durability::recheck(store, &self.receipt)?;
            // A fresh original-claim terminal check after actual live release
            // closes the last cancellation/revocation/publication window.
            let current = self.publication.observe_terminal(
                authority,
                &self.claim,
                &self.receipt,
                reply.as_ref(),
            )?;
            context.check()?;
            Ok((current, reply))
        })();
        match result {
            Ok((terminal, reply)) => Ok(VerifiedImportPublication {
                prepared: self,
                terminal,
                reply,
            }),
            Err(error) => {
                let _ = authority.unknown_import(&self.claim);
                Err(error)
            }
        }
    }
    pub fn prepare<C: NativeRenewalCancellation>(
        authority: &RemoteImportAuthority<'_, C>,
        context: &NativeImportInputs<'a>,
        store: &ExistingAccountImportStore,
        actor: &mut PreparedActor,
        mut validation: ImportValidation,
    ) -> Result<Self, GatewayError> {
        context.check_original()?;
        context.recheck_remote(authority)?;
        let candidate = validation.probe(context)?;
        context.check_original()?;
        context.recheck_remote(authority)?;
        if let ValidationOutcome::Valid(candidate) = &candidate {
            candidate.require_catalog(authority, context)?;
        }
        let mut claim = context.claim_remote(authority)?;
        if claim.mode() != NativeImportMode::ImportOnce {
            // A replay/current observation cannot stop a new daemon or issue a
            // new live session. Receipt-only reconciliation grants no mutation.
            return Err(unavailable());
        }
        let result = (|| {
            context.check_original()?;
            let start = context.start_remote(authority, &mut claim)?;
            let mut custody = actor.begin_custody(start, context)?;
            authority.recheck_import_claim(&claim)?;
            let candidate = match candidate {
                ValidationOutcome::Valid(candidate) => candidate,
                ValidationOutcome::Original401(pending) => {
                    store.refresh_once(&mut validation, pending, &mut custody, context)?
                }
            };
            candidate.require_catalog(authority, context)?;
            authority.recheck_import_claim(&claim)?;
            context.check_original()?;
            let receipt = durability::persist_live(store, &mut custody, context, &candidate)?;
            authority.recheck_import_claim(&claim)?;
            let publication = PreparedImportPublication::from_durable(
                store,
                &mut custody,
                context,
                &candidate,
                &receipt,
            )?;
            context.check_original()?;
            Ok((custody, candidate, receipt, publication))
        })();
        match result {
            Ok((custody, candidate, receipt, publication)) => Ok(Self {
                claim,
                custody,
                candidate,
                receipt,
                publication,
            }),
            Err(error) => {
                // The SAME original claim remains uncertain. A failed unknown
                // exchange is not permission to retry OAuth, stop or file writes.
                let _ = authority.unknown_import(&claim);
                Err(error)
            }
        }
    }
}
impl VerifiedImportPublication<'_> {
    pub fn source_result<C: NativeRenewalCancellation>(
        self,
        authority: &RemoteImportAuthority<'_, C>,
        context: &NativeImportInputs<'_>,
    ) -> Result<gateway_native_import_source::result::NativeImportResult, GatewayError> {
        let body = self.prepared.candidate.completion_body(
            authority,
            &self.prepared.claim,
            &self.terminal,
            context,
        )?;
        let completion = self
            .prepared
            .publication
            .completion_data(&self.terminal, self.reply.as_ref());
        Ok(gateway_native_import_source::result::NativeImportResult {
            status: 200,
            headers: std::collections::BTreeMap::from([(
                "x-uos-upstream".into(),
                "chatgpt_codex".into(),
            )]),
            body,
            completion,
        })
    }
}
