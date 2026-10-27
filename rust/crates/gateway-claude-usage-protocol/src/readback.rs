//! Source quota readback data projection. This codec confers no account authority.
use crate::{snapshot, state};
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source,
};

pub fn quota(
    value: &SourceJson,
    dates: &impl Fn(&JsString16) -> Option<i64>,
) -> Result<SourceJson, GatewayError> {
    let value = SourceJson::parse(value.as_str().trim())?;
    let mut fields = source::entries(&value).ok_or_else(|| {
        GatewayError::unavailable("Claude quota is not an object", "claude_quota_invalid")
    })?;
    let names = [
        "windows",
        "usage",
        "standard",
        "representative_claim",
        "blocked_until_ms",
        "block_window",
        "cooldown_until_ms",
        "last_error",
        "updated_at_ms",
    ];
    fields.retain(|(key, _)| names.iter().any(|name| key == &JsString16::from_utf8(name)));
    let usage = source::member(&value, "usage")
        .as_ref()
        .and_then(state::read);
    set(&mut fields, "usage", SourceJson::from_serializable(&usage)?);
    if let Some(snapshot) = source::member(&value, "usage_snapshot")
        .as_ref()
        .and_then(|value| snapshot::read(value, dates))
    {
        set(
            &mut fields,
            "usage_snapshot",
            snapshot::serialize(&snapshot)?,
        );
    }
    source::object_keys(fields)
}

fn set(fields: &mut Vec<(JsString16, SourceJson)>, name: &str, value: SourceJson) {
    let name = JsString16::from_utf8(name);
    if let Some((_, old)) = fields.iter_mut().find(|(key, _)| key == &name) {
        *old = value;
    } else {
        fields.push((name, value));
    }
}
