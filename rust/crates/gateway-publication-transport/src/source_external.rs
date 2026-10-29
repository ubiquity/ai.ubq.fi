//! Fixed Source external sender; no destination, connection or authority factory.
use gateway_auth_native_proof::native_auth::NativeTransportIdentity;
use gateway_auth_native_wire::native_read::{
    NATIVE_KEY_HEADER, NATIVE_MAC_HEADER, NATIVE_NONCE_HEADER, NATIVE_PUBLICATION_PATH,
    NATIVE_RESPONSE_MAC_HEADER, NATIVE_TIME_HEADER, NATIVE_VERSION_HEADER,
};
use gateway_auth_primitives::crypto::base64url;
use gateway_native_external_publication_wire::{
    command::PreparedSourceExternalAccessCommand, receipt::SourceExternalPublicationReceipt,
};
use gateway_publication_policy::protocol::{
    OwnedNativeAuthHeaders, PublicationCarrier, PublicationRequest, decode_response_error,
    unverified,
};
use gateway_source::error::GatewayError;
pub async fn send_source_external<C: PublicationCarrier>(
    identity: &NativeTransportIdentity,
    carrier: &C,
    command: &PreparedSourceExternalAccessCommand,
    nonce_bytes: [u8; 32],
    now_ms: i64,
) -> Result<SourceExternalPublicationReceipt, GatewayError> {
    let body = command.bytes()?;
    let nonce = base64url(&nonce_bytes);
    let signature = identity.sign(NATIVE_PUBLICATION_PATH, now_ms, &nonce, &body)?;
    let headers = OwnedNativeAuthHeaders::from_pairs(vec![
        (NATIVE_VERSION_HEADER.into(), "1".into()),
        (NATIVE_KEY_HEADER.into(), identity.key_id().into()),
        (NATIVE_TIME_HEADER.into(), now_ms.to_string()),
        (NATIVE_NONCE_HEADER.into(), nonce.clone()),
        (NATIVE_MAC_HEADER.into(), signature),
    ])?;
    let response = carrier
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
        || identity
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
    let receipt: SourceExternalPublicationReceipt =
        serde_json::from_slice(&response.raw_body).map_err(|_| unverified())?;
    if receipt.owner_id != identity.owner_id()
        || receipt.owner_epoch != identity.owner_epoch()
        || !command.matches(&receipt)
    {
        return Err(unverified());
    }
    Ok(receipt)
}
