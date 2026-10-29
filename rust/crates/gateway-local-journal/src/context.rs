//! Explicit ordinary-journal context DATA. Decoded rows never grant live authority.
use gateway_contracts::identity::{PrincipalId, RequestContext};
use gateway_source::{
    error::GatewayError, source_json::SourceJson, source_serialization as source,
};

/// Select only the actual logical journal version; physical value encoding is separate.
/// Callers still validate the complete journal, local phases, and their own currentness.
pub fn decode_journal_context(value: &SourceJson) -> Result<RequestContext, GatewayError> {
    let version = source::member(value, "v").and_then(|value| value.deserialize::<u64>().ok());
    let context = source::member(value, "context").ok_or_else(invalid)?;
    match version {
        Some(1 | 2) => context.deserialize(),
        Some(3) => RequestContext::from_source_json(&context),
        _ => Err(invalid()),
    }
}

/// Preserve the historical Rust scalar trim predicate on exact source UTF16 units.
pub fn principal_has_non_whitespace(principal: &PrincipalId) -> bool {
    principal.as_source().as_units().iter().any(|unit| {
        char::from_u32(u32::from(*unit)).is_none_or(|character| !character.is_whitespace())
    })
}

/// An explicitly annotated source context field, never the general RequestContext codec.
pub mod source_field {
    use gateway_contracts::identity::RequestContext;
    use gateway_source::source_json::SourceJson;
    use serde::{Deserialize, Deserializer, Serialize, Serializer};

    pub fn serialize<S: Serializer>(
        context: &RequestContext,
        serializer: S,
    ) -> Result<S::Ok, S::Error> {
        context
            .to_source_json()
            .map_err(serde::ser::Error::custom)?
            .serialize(serializer)
    }

    pub fn deserialize<'de, D: Deserializer<'de>>(
        deserializer: D,
    ) -> Result<RequestContext, D::Error> {
        let raw = <SourceJson as Deserialize<'de>>::deserialize(deserializer)?;
        RequestContext::from_source_json(&raw).map_err(serde::de::Error::custom)
    }
}

fn invalid() -> GatewayError {
    GatewayError::unavailable(
        "Routing journal context is invalid.",
        "routing_journal_context_invalid",
    )
}

/// Match the old JSON object equality without scalarizing opaque fields. Array
/// order and scalar number spellings remain significant; object key order does not.
pub fn same_source_value(left: &SourceJson, right: &SourceJson) -> bool {
    match (
        left.as_str().as_bytes().first(),
        right.as_str().as_bytes().first(),
    ) {
        (Some(b'{'), Some(b'{')) => {
            let (Some(left), Some(right)) = (source::entries(left), source::entries(right)) else {
                return false;
            };
            left.len() == right.len()
                && left.iter().all(|(key, value)| {
                    right
                        .iter()
                        .find(|(name, _)| name == key)
                        .is_some_and(|(_, other)| same_source_value(value, other))
                })
        }
        (Some(b'['), Some(b'[')) => {
            let (Some(left), Some(right)) = (source::array(left), source::array(right)) else {
                return false;
            };
            left.len() == right.len()
                && left
                    .iter()
                    .zip(right.iter())
                    .all(|(left, right)| same_source_value(left, right))
        }
        (Some(b'"'), Some(b'"')) => source::string(left) == source::string(right),
        _ => left.as_str() == right.as_str(),
    }
}
