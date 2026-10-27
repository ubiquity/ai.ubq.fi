use gateway_auth_primitives::error::invalid;
use gateway_core::error::GatewayError;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use std::future::Future;
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SignatureAlgorithm {
    Es256,
    Es384,
    Es512,
    Ed25519,
    RsaPkcs1Sha256,
    RsaPkcs1Sha384,
    RsaPkcs1Sha512,
    RsaPkcs1Sha1,
    RsaPssSha256,
    RsaPssSha384,
    RsaPssSha512,
}
impl SignatureAlgorithm {
    pub fn import_algorithm(self) -> Value {
        match self {
            Self::Es256 => json!({"name":"ECDSA","namedCurve":"P-256"}),
            Self::Es384 => json!({"name":"ECDSA","namedCurve":"P-384"}),
            Self::Es512 => json!({"name":"ECDSA","namedCurve":"P-521"}),
            Self::Ed25519 => json!({"name":"Ed25519"}),
            Self::RsaPssSha256 | Self::RsaPssSha384 | Self::RsaPssSha512 => {
                json!({"name":"RSA-PSS","hash":{"name":self.hash_name()}})
            }
            _ => json!({"name":"RSASSA-PKCS1-v1_5","hash":{"name":self.hash_name()}}),
        }
    }
    pub fn verify_algorithm(self) -> Value {
        match self {
            Self::Es256 | Self::Es384 | Self::Es512 => {
                json!({"name":"ECDSA","hash":{"name":self.hash_name()}})
            }
            Self::Ed25519 => json!({"name":"Ed25519"}),
            Self::RsaPssSha256 | Self::RsaPssSha384 | Self::RsaPssSha512 => {
                json!({"name":"RSA-PSS","saltLength":self.pss_salt_length()})
            }
            _ => json!({"name":"RSASSA-PKCS1-v1_5"}),
        }
    }
    pub fn cose_id(self) -> i64 {
        match self {
            Self::Es256 => -7,
            Self::Es384 => -35,
            Self::Es512 => -36,
            Self::Ed25519 => -8,
            Self::RsaPkcs1Sha256 => -257,
            Self::RsaPkcs1Sha384 => -258,
            Self::RsaPkcs1Sha512 => -259,
            Self::RsaPkcs1Sha1 => -65535,
            Self::RsaPssSha256 => -37,
            Self::RsaPssSha384 => -38,
            Self::RsaPssSha512 => -39,
        }
    }
    pub fn parse(id: i64) -> Result<Self, GatewayError> {
        match id {
            -7 => Ok(Self::Es256),
            -35 => Ok(Self::Es384),
            -36 => Ok(Self::Es512),
            -8 => Ok(Self::Ed25519),
            -257 => Ok(Self::RsaPkcs1Sha256),
            -258 => Ok(Self::RsaPkcs1Sha384),
            -259 => Ok(Self::RsaPkcs1Sha512),
            -65535 => Ok(Self::RsaPkcs1Sha1),
            -37 => Ok(Self::RsaPssSha256),
            -38 => Ok(Self::RsaPssSha384),
            -39 => Ok(Self::RsaPssSha512),
            _ => Err(invalid("Unsupported passkey public key algorithm")),
        }
    }
    pub fn hash_name(self) -> Option<&'static str> {
        match self {
            Self::Es256 | Self::RsaPkcs1Sha256 | Self::RsaPssSha256 => Some("SHA-256"),
            Self::Es384 | Self::RsaPkcs1Sha384 | Self::RsaPssSha384 => Some("SHA-384"),
            Self::Es512 | Self::RsaPkcs1Sha512 | Self::RsaPssSha512 => Some("SHA-512"),
            Self::RsaPkcs1Sha1 => Some("SHA-1"),
            Self::Ed25519 => None,
        }
    }
    pub fn pss_salt_length(self) -> Option<u32> {
        match self {
            Self::RsaPssSha256 => Some(32),
            Self::RsaPssSha384 => Some(48),
            Self::RsaPssSha512 => Some(64),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct CosePublicKey {
    pub algorithm: SignatureAlgorithm,
    pub jwk: Value,
}
impl CosePublicKey {
    /// Source EC verification selects the real curve independently of the COSE hash ID.
    pub fn import_algorithm(&self) -> Value {
        if self.jwk["kty"] == "EC" {
            json!({"name":"ECDSA","namedCurve":self.jwk["crv"]})
        } else {
            self.algorithm.import_algorithm()
        }
    }
    pub fn verify_algorithm(&self) -> Value {
        if self.jwk["kty"] == "EC" {
            json!({"name":"ECDSA","hash":{"name":self.algorithm.hash_name()}})
        } else {
            self.algorithm.verify_algorithm()
        }
    }
}
#[derive(Clone, Debug)]
pub struct SignatureInput {
    pub key: CosePublicKey,
    pub signature: Vec<u8>,
    pub data: Vec<u8>,
}

pub trait WebAuthnCrypto {
    /// Raw DER from the signed certificate's untrusted first CRL URL. Source semantics
    /// ignore unavailable/malformed CRLs; only Rust parsing decides revocation.
    fn fetch_certificate_revocation_list(
        &self,
        _url: &str,
    ) -> impl Future<Output = Result<Vec<u8>, GatewayError>> {
        async {
            Err(GatewayError::unavailable(
                "CRL transport unavailable",
                "passkey_crl_unavailable",
            ))
        }
    }
    /// Real cryptographic key import validates curve points/RSA parameters; no Boolean-policy callback.
    fn validate_key(&self, key: &CosePublicKey) -> impl Future<Output = Result<(), GatewayError>>;
    /// ECDSA signatures are already fixed-width IEEE P1363, as required by WebCrypto.
    fn verify_signature(
        &self,
        input: SignatureInput,
    ) -> impl Future<Output = Result<bool, GatewayError>>;
}
