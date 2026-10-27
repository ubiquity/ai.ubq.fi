use gateway_source::{error::GatewayError, js_string::JsString16};
use gateway_schema::aliases::SourceStringCollator;
use std::cmp::Ordering;

pub struct FixtureCollator {
    pub order: Vec<String>,
    pub failed: bool,
}
impl SourceStringCollator for FixtureCollator {
    fn compare(&self, left: &JsString16, right: &JsString16) -> Result<Ordering, GatewayError> {
        if self.failed {
            return Err(GatewayError::unavailable(
                "Locale comparator unavailable",
                "fixture_collation",
            ));
        }
        let rank = |value: &JsString16| {
            self.order
                .iter()
                .position(|id| value.as_units().iter().copied().eq(id.encode_utf16()))
                .unwrap_or_else(|| panic!("locale case absent from source oracle"))
        };
        Ok(rank(left).cmp(&rank(right)))
    }
}
