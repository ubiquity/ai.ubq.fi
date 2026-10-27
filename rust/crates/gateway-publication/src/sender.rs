use crate::wire::{PreparedPublicationCommand, PublicationReceipt};
use gateway_auth::crypto::base64url;
use gateway_auth::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_PUBLICATION_PATH,
    NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
};
use gateway_core::error::GatewayError;
use gateway_publication_policy::protocol::{
    OwnedNativeAuthHeaders, PublicationCarrier, PublicationRequest, decode_response_error,
    unverified,
};

pub struct NativePublicationSender<'a, C> {
    pub(crate) identity: &'a NativeTransportIdentity,
    pub(crate) carrier: &'a C,
}
impl<'a, C: PublicationCarrier> NativePublicationSender<'a, C> {
    pub fn configured(identity: &'a NativeTransportIdentity, carrier: &'a C) -> Self {
        Self { identity, carrier }
    }
    pub async fn send(
        &self,
        command: &PreparedPublicationCommand,
        nonce_bytes: [u8; 32],
        now_ms: i64,
    ) -> Result<PublicationReceipt, GatewayError> {
        let body = command.0.bytes()?;
        let nonce = base64url(&nonce_bytes);
        let signature = self
            .identity
            .sign(NATIVE_PUBLICATION_PATH, now_ms, &nonce, &body)?;
        let headers = OwnedNativeAuthHeaders::from_pairs(vec![
            (NATIVE_VERSION_HEADER.into(), "1".into()),
            (NATIVE_KEY_HEADER.into(), self.identity.key_id().into()),
            (NATIVE_TIME_HEADER.into(), now_ms.to_string()),
            (NATIVE_NONCE_HEADER.into(), nonce.clone()),
            (NATIVE_MAC_HEADER.into(), signature),
        ])?;
        let response = self
            .carrier
            .send(PublicationRequest {
                method: "POST".into(),
                path: NATIVE_PUBLICATION_PATH.into(),
                raw_body: body.clone(),
                headers,
            })
            .await?;
        let signatures: Vec<_> = response
            .headers
            .iter()
            .filter(|(name, _)| name.eq_ignore_ascii_case(NATIVE_RESPONSE_MAC_HEADER))
            .collect();
        if signatures.len() != 1
            || self
                .identity
                .verify_publication_response(
                    &nonce,
                    &body,
                    response.status,
                    &response.raw_body,
                    &signatures[0].1,
                )
                .is_err()
        {
            return Err(unverified());
        }
        if response.status != 200 {
            return Err(decode_response_error(response.status, &response.raw_body)?);
        }
        let receipt: PublicationReceipt =
            serde_json::from_slice(&response.raw_body).map_err(|_| unverified())?;
        if receipt.owner_id != self.identity.owner_id()
            || receipt.owner_epoch != self.identity.owner_epoch()
            || !receipt.matches(&command.0)
        {
            return Err(unverified());
        }
        Ok(receipt)
    }
}
