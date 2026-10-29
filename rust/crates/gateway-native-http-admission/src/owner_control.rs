//! Fixed trusted owner renewal handler; checked contexts come from the actual authenticator.
use gateway_auth_native_proof::{
    native_auth::AuthenticatedNativeOwner, native_context::NativeControlContext,
};
use gateway_auth_native_wire::control::{NativeControlEnvelope, NativeControlResponse};
use gateway_source::error::GatewayError;
/// This is not a deserializable authorization or a generic operation callback.
/// Implementations must reconstruct/claim the prepared action in the SAME
/// authority, check before dispatch and after awaits, and reconcile an unknown
/// original operation without invoking another upstream refresh.
pub trait NativeOwnerRenewalHandler: Send + Sync {
    fn renew(
        &self,
        proof: &AuthenticatedNativeOwner,
        request: &NativeControlEnvelope,
        raw_body: &[u8],
        context: &NativeControlContext<'_>,
    ) -> Result<NativeControlResponse, GatewayError>;
}
