/// Key encoding is owned by the versioned portable-state adapter, not a legacy KV codec.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RecordKey(pub String);
