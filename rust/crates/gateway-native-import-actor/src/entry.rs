use crate::transaction::PreparedExistingImport;
use gateway_auth_native_proof::native_context::NativeRequestLifetime;
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_import_request::setup::{BeforeNativeStore, PreparedActor};
use gateway_native_import_store::store::ExistingAccountImportStore;
use gateway_native_import_validation::validation::ImportValidation;
use gateway_source::error::GatewayError;

/// This private owner implements the original inverse local cleanup order across crates.
struct RequestScope {
    actor: Option<PreparedActor>,
    store: Option<ExistingAccountImportStore>,
}
impl Drop for RequestScope {
    fn drop(&mut self) {
        let tail = self.actor.take().map(PreparedActor::release_nonces);
        drop(self.store.take());
        drop(tail);
    }
}
pub fn run_native_import() -> ! {
    if gateway_native_connector::artifact::require_production_artifact().is_err()
        || !cfg!(target_os = "macos")
    {
        std::process::exit(78);
    }
    exit_actor()
}
#[cfg(feature = "native-import-fixture")]
pub fn run_native_import_fixture() -> ! {
    exit_actor()
}
fn exit_actor() -> ! {
    let code = if run_actor().is_ok() { 0 } else { 1 };
    // Stderr remains the actual custody endpoint, never credential diagnostics.
    std::process::exit(code)
}
fn run_actor() -> Result<(), GatewayError> {
    if !cfg!(target_os = "macos") || std::env::args_os().count() != 1 {
        return Err(unavailable());
    }
    let (before, input) = BeforeNativeStore::from_stdio()?;
    let store = ExistingAccountImportStore::configured(&input.registered, input.account_id_hash)?;
    let (actor, temporary_cancellation, deadline) = match before.resume() {
        Ok(ready) => ready,
        Err(failure) => {
            let (error, tail) = failure.release_before_store();
            drop(store);
            drop(tail);
            return Err(error);
        }
    };
    let mut scope = RequestScope {
        actor: Some(actor),
        store: Some(store),
    };
    // Declared after the scope: the real cancellation clone drops before nonce/Store/transports.
    let cancellation = temporary_cancellation;
    let result = (|| {
        let actor = scope.actor.as_mut().ok_or_else(unavailable)?;
        let store = scope.store.as_ref().ok_or_else(unavailable)?;
        let head = actor.read_head()?;
        let cancelled = || cancellation.cancelled();
        let lifetime = NativeRequestLifetime::until(deadline, &cancelled);
        lifetime.check()?;
        let (authority, connected) = actor.connect_authority(&cancellation, deadline)?;
        let (document, original) = actor.read_original(&head, &authority, lifetime)?;
        let bound = document.decode_original(&original)?;
        let context = bound.check_inputs()?;
        let version = context.client_version_data();
        let (clients, remaining) = connected.prepare_validation(version.as_ref())?;
        let validation = ImportValidation::from_clients(clients);
        let sender = actor.prepare_publication(remaining)?;
        let prepared =
            PreparedExistingImport::prepare(&authority, &context, store, actor, validation)?;
        let verified = prepared.publish(&authority, &sender, store, &context)?;
        let result = verified.source_result(&authority, &context)?;
        actor.write_result(result)
    })();
    match result {
        Ok(()) => Ok(()),
        Err(error) => scope
            .actor
            .as_mut()
            .ok_or_else(unavailable)?
            .write_error(error),
    }
}
