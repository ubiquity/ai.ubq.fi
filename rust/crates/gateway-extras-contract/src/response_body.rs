//! Buffered response DATA; no admission, dispatch or settlement operation.
use gateway_local_refusal::closed_error_body::ClosedErrorBody;
use gateway_source::source_json::SourceJson;

#[derive(Clone, Debug)]
pub enum ExtrasResponseBody {
    Json(SourceJson),
    ClosedError(ClosedErrorBody),
}
impl ExtrasResponseBody {
    pub fn as_str(&self) -> &str {
        match self {
            Self::Json(body) => body.as_str(),
            Self::ClosedError(body) => body.as_str(),
        }
    }
    pub fn as_bytes(&self) -> &[u8] {
        self.as_str().as_bytes()
    }
}
impl PartialEq for ExtrasResponseBody {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}
impl Eq for ExtrasResponseBody {}
impl serde::Serialize for ExtrasResponseBody {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Self::Json(body) => serde::Serialize::serialize(body, serializer),
            Self::ClosedError(body) => serde::Serialize::serialize(body, serializer),
        }
    }
}
