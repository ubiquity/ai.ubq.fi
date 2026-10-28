//! One fixed private native publication route. The HTTP adapter only carries
//! bytes; this receiver owns parsing, authentication, durable fences and CAS.
use gateway_auth_native_contracts::transport::NativeAuthHeaders;
use gateway_auth_native_proof::native_auth::{NativeAuthenticator, NativeTransportIdentity};
use gateway_auth_native_protocol::native_replay::StoreNonceAuthority;
use gateway_auth_native_wire::native_read::{
    NATIVE_MAX_BODY_BYTES, NATIVE_PUBLICATION_PATH, NATIVE_RESPONSE_MAC_HEADER,
};
use gateway_core::state::AuthorityStore;
use gateway_publication::access_publication::RegisteredAccessOwner;
use gateway_publication_policy::control_publication::ConfiguredPaidInputs;
use gateway_publication_policy::protocol::{
    PublicationRequest, PublicationResponse, error_response, invalid, unverified,
};
use gateway_source::error::GatewayError;

pub struct PublicationReceiver {
    identity: NativeTransportIdentity,
    inputs: ConfiguredPaidInputs,
    registered: Vec<RegisteredAccessOwner>,
    clock: fn() -> i64,
}
impl PublicationReceiver {
    pub fn configured(
        identity: NativeTransportIdentity,
        inputs: ConfiguredPaidInputs,
        registered_access_owners: Vec<RegisteredAccessOwner>,
        clock: fn() -> i64,
    ) -> Result<Self, GatewayError> {
        let mut seen = std::collections::BTreeSet::new();
        if registered_access_owners.len() > 2
            || registered_access_owners
                .iter()
                .any(|owner| !seen.insert(owner.native_owner_id().0.clone()))
        {
            return Err(invalid());
        }
        Ok(Self {
            identity,
            inputs,
            registered: registered_access_owners,
            clock,
        })
    }
    pub(crate) fn import_identity(&self) -> &NativeTransportIdentity {
        &self.identity
    }
    pub(crate) fn import_inputs(&self) -> &ConfiguredPaidInputs {
        &self.inputs
    }
    pub async fn handle<S: AuthorityStore>(
        &self,
        store: &S,
        request: PublicationRequest,
    ) -> PublicationResponse {
        if request.method != "POST" || request.path != NATIVE_PUBLICATION_PATH {
            return error_response(GatewayError::new(
                404,
                "Unknown native publication route",
                Some("native_publication_route_invalid"),
            ));
        }
        if request.raw_body.len() > NATIVE_MAX_BODY_BYTES {
            return error_response(GatewayError::new(
                413,
                "Native publication body exceeds its limit",
                Some("native_publication_body_too_large"),
            ));
        }
        let header_pairs = request.headers.pairs();
        let replay = StoreNonceAuthority::new(store);
        let authenticator = NativeAuthenticator::new(&self.identity, &replay);
        let proof = match authenticator
            .authenticate(
                NATIVE_PUBLICATION_PATH,
                borrowed_headers(&header_pairs),
                &request.raw_body,
                (self.clock)(),
            )
            .await
        {
            Ok(proof) => proof,
            Err(error) => return error_response(error),
        };
        let result = gateway_publication::apply::apply_publication(
            store,
            &proof,
            &request.raw_body,
            &self.identity,
            &self.inputs,
            &self.registered,
            self.clock,
        )
        .await;
        let mut response = match result {
            Ok(receipt) => match serde_json::to_vec(&receipt) {
                Ok(raw_body) => PublicationResponse {
                    status: 200,
                    headers: vec![
                        ("content-type".into(), "application/json".into()),
                        ("cache-control".into(), "no-store".into()),
                    ],
                    raw_body,
                },
                Err(_) => error_response(unverified()),
            },
            Err(error) => error_response(error),
        };
        match self.identity.sign_publication_response(
            &header_pairs[3].1,
            &request.raw_body,
            response.status,
            &response.raw_body,
        ) {
            Ok(mac) => response
                .headers
                .push((NATIVE_RESPONSE_MAC_HEADER.into(), mac)),
            Err(error) => return error_response(error),
        }
        response
    }
}

pub(crate) fn borrowed_headers(pairs: &[(String, String)]) -> NativeAuthHeaders<'_> {
    NativeAuthHeaders {
        version: &pairs[0].1,
        key_id: &pairs[1].1,
        time_ms: &pairs[2].1,
        nonce: &pairs[3].1,
        mac: &pairs[4].1,
    }
}
