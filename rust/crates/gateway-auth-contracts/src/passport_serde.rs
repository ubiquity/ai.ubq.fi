//! Historical AuthMethod scalar serde stays exact. New source/app DATA uses its
//! explicit v2 codecs; it cannot be silently flattened into an old JWT schema.
use gateway_auth_passport_contracts::{
    actor_data::PassportSessionData, passport::PassportClaims, session::PassportSourceClaims,
};
use serde::{Deserialize, Deserializer, Serialize, Serializer};
pub(crate) mod jwt {
    use super::{
        Deserialize, Deserializer, PassportClaims, PassportSourceClaims, Serialize, Serializer,
    };
    pub fn serialize<S: Serializer>(
        value: &PassportSourceClaims,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        value
            .try_legacy_scalar()
            .map_err(serde::ser::Error::custom)?
            .serialize(serializer)
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<PassportSourceClaims, D::Error> {
        PassportClaims::deserialize(deserializer).map(PassportSourceClaims::from_legacy)
    }
}
pub(crate) mod session {
    use super::{Deserializer, PassportSessionData, Serializer};
    pub fn serialize<S: Serializer>(
        value: &PassportSessionData,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        match value {
            PassportSessionData::Jwt(value) => super::jwt::serialize(value, serializer),
            PassportSessionData::App(_) => Err(serde::ser::Error::custom(
                "Persistent Passport DATA requires its explicit v2 schema",
            )),
        }
    }
    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<PassportSessionData, D::Error> {
        super::jwt::deserialize(deserializer).map(PassportSessionData::Jwt)
    }
}
