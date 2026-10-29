use crate::{
    receipt::SourceExternalPublicationReceipt,
    request::{ExternalAccessDocument, SourceExternalAccessRequest, invalid, native},
};
use gateway_auth_credentials::native_credentials::NativeCredentials;
use gateway_native_authority_wire::source_fence::SourceNativeRenewalFenceV3;
use gateway_source::error::GatewayError;
pub struct ExpectedSourceExternalAccessPublication<'a> {
    pub fence: &'a SourceNativeRenewalFenceV3,
    pub action_ref: &'a str,
    pub operation_id: &'a str,
    pub claim_ref: &'a str,
    pub publication_revision: &'a str,
}
pub struct PreparedSourceExternalAccessCommand {
    request: SourceExternalAccessRequest,
}
impl PreparedSourceExternalAccessCommand {
    pub fn from_registered(
        expected: ExpectedSourceExternalAccessPublication<'_>,
        observed: &NativeCredentials,
    ) -> Result<Self, GatewayError> {
        let fence = expected.fence;
        if fence.v != 3
            || observed.routing_account_hash() != fence.account_id_hash
            || !observed.refresh_token.expose().is_empty()
        {
            return Err(invalid());
        }
        let document = ExternalAccessDocument {
            account_id: observed.account_id.clone(),
            access_token: observed.access_token.expose().into(),
            user_id: observed.external_user_id.clone().ok_or_else(invalid)?,
            native_refreshed_at: observed.native_refreshed_at.clone(),
        };
        let parsed = native(&document)?;
        if !parsed.same_credentials(observed)
            || parsed.external_user_id != observed.external_user_id
        {
            return Err(invalid());
        }
        let request = SourceExternalAccessRequest {
            operation: "source_external_access_publish".into(),
            v: 2,
            action_ref: expected.action_ref.into(),
            operation_id: expected.operation_id.into(),
            claim_ref: expected.claim_ref.into(),
            native_owner_id: fence.native_owner_id.clone(),
            owner_revision: fence.owner_revision.clone(),
            source_revision: fence.source_revision.clone(),
            normalized_revision: fence.normalized_revision.clone(),
            publication_revision: expected.publication_revision.into(),
            account_id_hash: observed.routing_account_hash(),
            credential_version: observed.routing_credential_version(),
            previous_native_source_generation: fence.source_native_generation.clone(),
            native_generation: observed.generation_hash(),
            document,
        };
        let command = Self { request };
        SourceExternalAccessRequest::parse(&command.bytes()?)?;
        Ok(command)
    }
    pub fn bytes(&self) -> Result<Vec<u8>, GatewayError> {
        serde_json::to_vec(&self.request).map_err(|_| invalid())
    }
    pub fn matches(&self, receipt: &SourceExternalPublicationReceipt) -> bool {
        receipt.matches(&self.request)
    }
}
