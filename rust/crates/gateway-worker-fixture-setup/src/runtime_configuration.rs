//! Complete real fixed configuration publication after ordinary runtime bootstrap.
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_publication_transport::transport::PublicationReceiver;
use gateway_source::error::GatewayError;
use gateway_worker_authority_binding::authority::RemoteAuthorityStore;
struct Carrier<'a> {
    store: &'a RemoteAuthorityStore,
    receiver: &'a PublicationReceiver,
}
impl gateway_publication_policy::protocol::PublicationCarrier for Carrier<'_> {
    async fn send(
        &self,
        request: gateway_publication_policy::protocol::PublicationRequest,
    ) -> Result<gateway_publication_policy::protocol::PublicationResponse, GatewayError> {
        Ok(self.receiver.handle(self.store, request).await)
    }
}
pub async fn publish(
    store: &RemoteAuthorityStore,
    id: &NativeTransportIdentity,
    inputs: &gateway_publication_policy::control_publication::ConfiguredPaidInputs,
    receiver: &PublicationReceiver,
) -> Result<(), GatewayError> {
    let now = gateway_worker_storage::store::now_ms()?;
    let prepared =
        gateway_publication_policy::control_publication::PreparedConfigurationPublication::capture(
            store,
            id,
            inputs,
            "fixture-external-runtime-v1",
            now,
        )
        .await?;
    let revision = gateway_worker_platform::random::opaque_token()?;
    let command = gateway_publication::wire::configuration_command(&prepared, &revision)?;
    let carrier = Carrier { store, receiver };
    let nonce = [229; 32];
    gateway_publication::sender::NativePublicationSender::configured(id, &carrier)
        .send(&command, nonce, now)
        .await?;
    Ok(())
}
