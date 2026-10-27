//! Native read adapter only; portable publication policy has one implementation.
use gateway_auth_credentials::native_credentials::{NativeCredentials, NativeOwnerId};
use gateway_auth_native_proof::native_auth::{AuthenticatedNativeOwner, NativeTransportIdentity};
use gateway_authority_routing_records::types::SubscriptionPool;
use gateway_core::state::AuthorityStore;
use gateway_native_local_transport::transport::NativeOwnerTransport;
use gateway_publication::access_publication::{AccessCredentialSource, RegisteredAccessOwner};
use gateway_source::error::GatewayError;

struct NativeAccessSource<'a, T>(&'a T);
impl<T: NativeOwnerTransport> AccessCredentialSource for NativeAccessSource<'_, T> {
    async fn read_current_access(
        &self,
        owner: &NativeOwnerId,
    ) -> Result<NativeCredentials, GatewayError> {
        self.0.read_credentials(owner).await
    }
}

pub struct PreparedAccessPublication(
    gateway_publication::access_publication::PreparedAccessPublication,
);
impl PreparedAccessPublication {
    pub async fn capture<S: AuthorityStore, T: NativeOwnerTransport>(
        store: &S,
        transport: &T,
        identity: &NativeTransportIdentity,
        registered: &RegisteredAccessOwner,
        now_ms: i64,
        revision: &str,
    ) -> Result<Self, GatewayError> {
        gateway_publication::access_publication::PreparedAccessPublication::capture(
            store,
            &NativeAccessSource(transport),
            identity,
            registered,
            now_ms,
            revision,
        )
        .await
        .map(Self)
    }
    pub fn statement(&self) -> &[u8] {
        self.0.statement()
    }
    pub async fn publish<S: AuthorityStore>(
        self,
        store: &S,
        proof: &AuthenticatedNativeOwner,
        now_ms: i64,
    ) -> Result<SubscriptionPool, GatewayError> {
        self.0.publish(store, proof, now_ms).await
    }
}
