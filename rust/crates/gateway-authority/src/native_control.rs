//! Fixed owner-renewal coordination in the existing authority store.
//! Wire selectors and fence DTOs are observations, never permissions to mutate native state.
use gateway_auth_native_proof::native_auth::AuthenticatedNativeOwner;
use gateway_native_authority_wire::renewal::{NativeRenewalFence, NativeRenewalMode};
use gateway_source::error::GatewayError;
use std::future::Future;

pub mod actor;
pub mod custody;
mod facts;
pub mod import;
mod live_renewal;
pub mod managed;
pub mod owners;
pub mod receiver;
mod rows;
pub mod wire;

pub const OWNER_RENEWAL_MAX_MS: i64 = 45_000;

/// This capability is minted only after server authentication and fixed strong-read/CAS checks.
/// ```compile_fail
/// use gateway_authority::native_control::NativeRenewalAction;
/// let forged: NativeRenewalAction = serde_json::from_str("{}").unwrap();
/// ```
pub struct NativeRenewalAction {
    action_ref: String,
    deadline_ms: i64,
    fence: NativeRenewalFence,
}

impl NativeRenewalAction {
    pub fn action_ref(&self) -> &str {
        &self.action_ref
    }
    pub fn deadline_ms(&self) -> i64 {
        self.deadline_ms
    }
    pub fn fence(&self) -> &NativeRenewalFence {
        &self.fence
    }
}

/// A transport may carry its opaque reference; deserializing a selector cannot mint this capability.
/// ```compile_fail
/// use gateway_authority::native_control::NativeRenewalClaim;
/// let forged: NativeRenewalClaim = serde_json::from_str("{}").unwrap();
/// ```
pub struct NativeRenewalClaim {
    claim_ref: String,
    operation_id: String,
    action: NativeRenewalAction,
    mode: NativeRenewalMode,
    start_permit: Option<String>,
    publication_revision: Option<String>,
}

impl NativeRenewalClaim {
    pub fn claim_ref(&self) -> &str {
        &self.claim_ref
    }
    pub fn operation_id(&self) -> &str {
        &self.operation_id
    }
    pub fn action(&self) -> &NativeRenewalAction {
        &self.action
    }
    pub fn mode(&self) -> NativeRenewalMode {
        self.mode
    }
    /// Returned once to the private authenticated carrier; never present on replay.
    pub fn start_permit(&self) -> Option<&str> {
        self.start_permit.as_deref()
    }
    pub fn publication_revision(&self) -> Option<&str> {
        self.publication_revision.as_deref()
    }
}

/// Only a verified current durable publication produces this result.
pub struct NativeRenewalPublished {
    publication_revision: String,
}
impl NativeRenewalPublished {
    pub fn publication_revision(&self) -> &str {
        &self.publication_revision
    }
}

/// Constructor-owned cancellation, never a Boolean accepted from an HTTP request.
pub trait NativeRenewalCancellation {
    fn cancelled(&self) -> bool;
}

/// Preparation authenticates the original request through AuthService on the same AuthorityStore.
/// Reverse calls resolve only captured nonsecret current fences; they never need the original bearer.
/// Durable rows contain only secretless checks/digests, never the original credential or request body.
/// This trait defines the finite receiver/transport seam; it grants no generic store or native RPC access.
pub trait NativeRenewalControl {
    /// Initial native authentication must bind the exact control envelope/body and OwnerRenewal route.
    /// The implementation resolves the prepared action and atomically binds operation ID + digest.
    fn claim(
        &self,
        proof: &AuthenticatedNativeOwner,
        action_ref: &str,
        operation_id: &str,
        exact_body: &[u8],
    ) -> impl Future<Output = Result<NativeRenewalClaim, GatewayError>>;

    /// Commit possible-effect intent before account/read(true). A second call never returns RenewOnce.
    fn start(
        &self,
        claim: &mut NativeRenewalClaim,
    ) -> impl Future<Output = Result<NativeRenewalMode, GatewayError>>;

    /// Re-read principal/action/admission, owner and source fences after every awaited boundary.
    fn recheck(
        &self,
        claim: &NativeRenewalClaim,
    ) -> impl Future<Output = Result<NativeRenewalFence, GatewayError>>;

    /// Independently validate the existing publication and current source/normalized rows.
    fn published(
        &self,
        claim: &NativeRenewalClaim,
        publication_revision: &str,
    ) -> impl Future<Output = Result<NativeRenewalPublished, GatewayError>>;

    /// Unknown state has no TTL and never authorizes another renewal, even after caller/deadline loss.
    fn unknown(&self, claim: &NativeRenewalClaim)
    -> impl Future<Output = Result<(), GatewayError>>;
}
