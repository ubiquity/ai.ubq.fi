use crate::context::NativeImportInputs;
use gateway_auth_native_contracts::native_import::{NativeImportKind, NativeImportMetadata};
use gateway_auth_native_proof::{
    native_auth::NativeTransportIdentity,
    native_context::{NativeControlContext, NativeRequestLifetime},
};
use gateway_authority_contracts::NativeRenewalCancellation;
use gateway_native_custody_io::protocol::unavailable;
use gateway_native_http_framing::{
    import_body::{RawImportDocument, read_import_body},
    import_head::ImportHead,
};
use gateway_native_replay::nonce_sqlite::SqliteNonceStore;
use gateway_source::error::GatewayError;
use std::net::TcpStream;

struct OriginalCancellation<'a, 'b>(&'a NativeControlContext<'b>);
impl NativeRenewalCancellation for OriginalCancellation<'_, '_> {
    fn cancelled(&self) -> bool {
        self.0.cancelled()
    }
}
/// Original accepted request DATA and genuine actual-remote proof. No decoder or
/// second nonce store is introduced for the dedicated actor.
pub struct RemoteImportDocument {
    proof: gateway_authority::native_control::import::AuthenticatedNativeImport,
    metadata: NativeImportMetadata,
    raw: RawImportDocument,
}
impl RemoteImportDocument {
    fn body(&self) -> &[u8] {
        self.raw.as_bytes()
    }
}
pub fn read_remote_import<'c, C: NativeRenewalCancellation>(
    stream: &mut TcpStream,
    head: &ImportHead,
    identity: &NativeTransportIdentity,
    nonces: &SqliteNonceStore,
    authority: &gateway_authority::native_control::import::remote::RemoteImportAuthority<'_, C>,
    lifetime: NativeRequestLifetime<'c>,
    registered_target: (&str, &str),
) -> Result<(RemoteImportDocument, NativeControlContext<'c>), GatewayError> {
    let (native_owner_id, account_id_hash) = registered_target;
    let (metadata, header, original) =
        gateway_native_http_admission::import_ingress::admit_import_head(
            head, identity, nonces, lifetime,
        )?;
    let admitted = authority.admit_import(header)?;
    original.check()?;
    if admitted.native_owner_id() != native_owner_id
        || admitted.account_id_hash() != account_id_hash
        || !matches!(admitted.target(), gateway_auth_native_contracts::native_import::NativeImportTarget::ExistingAccount { account_id_hash: actual } if actual == account_id_hash)
    {
        return Err(unavailable());
    }
    if head.kind != NativeImportKind::CodexAuth || head.path != head.kind.path() {
        return Err(unavailable());
    }
    let raw = read_import_body(stream, head, &OriginalCancellation(&original))?;
    original.check()?;
    let proof = authority.finish_import(admitted, raw.as_bytes())?;
    authority.recheck_import(&proof)?;
    original.check()?;
    Ok((
        RemoteImportDocument {
            proof,
            metadata,
            raw,
        },
        original,
    ))
}

pub struct BoundOriginalImport<'a> {
    document: &'a RemoteImportDocument,
    decoded: gateway_native_http_admission::native_import::NativeImportContext<'a>,
    original: &'a NativeControlContext<'a>,
}
impl RemoteImportDocument {
    pub fn decode_original<'a>(
        &'a self,
        original: &'a NativeControlContext<'a>,
    ) -> Result<BoundOriginalImport<'a>, GatewayError> {
        let decoded = gateway_native_http_admission::native_import::decode_context(
            &self.proof,
            &self.metadata,
            self.body(),
            original,
        )?;
        Ok(BoundOriginalImport {
            document: self,
            decoded,
            original,
        })
    }
}
impl BoundOriginalImport<'_> {
    pub fn check_inputs(&self) -> Result<NativeImportInputs<'_>, GatewayError> {
        NativeImportInputs::checked(
            &self.document.proof,
            &self.document.metadata,
            self.document.body(),
            &self.decoded,
            self.original,
        )
    }
}
