//! Remaining source cache metadata/capability definitions; transferred request names are not forwarded.
macro_rules! json {
    ({$($key:literal:$value:expr),*$(,)?})=>{gateway_schema_values::value::Value::object(vec![$((gateway_source::js_string::JsString16::from_utf8($key),gateway_schema_values::value::Value::from_serializable(&$value).expect("Source normalization fields serialize"))),*])};
    ($value:expr)=>{gateway_schema_values::value::Value::from_serializable(&$value).expect("Source normalization value serializes")};
}
pub mod cache_capability;
pub(crate) mod cache_metadata;
