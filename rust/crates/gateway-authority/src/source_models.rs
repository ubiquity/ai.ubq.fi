//! Adapt the one raw source account-model record; never persist normalized hash-keyed hints there.
use crate::invalid;
use gateway_core::{
    error::GatewayError, model::ModelId, source_json::SourceJson, source_serialization as source,
};
use gateway_schema::selection::AccountModelsStore;

pub(crate) fn learn(
    raw: &SourceJson,
    id: &str,
    model: &ModelId,
    now: i64,
    collator: &dyn gateway_schema::aliases::SourceStringCollator,
) -> Result<SourceJson, GatewayError> {
    let parsed = AccountModelsStore::parse(raw)
        .unwrap_or_default()
        .record_unsupported(id, model, now, collator)?;
    let mut next = raw.clone();
    if source::entries(&next).is_none() {
        return Err(invalid(
            "Cannot update a malformed source account-model record.",
        ));
    }
    let mut unsupported = source::member(raw, "unsupported")
        .unwrap_or_else(|| SourceJson::parse("{}").expect("fixed empty source object"));
    if source::entries(&unsupported).is_none() {
        return Err(invalid("Source unsupported-model map is malformed."));
    }
    let entries = parsed
        .unsupported
        .get(id)
        .ok_or_else(|| invalid("Source unsupported-model observation is missing."))?;
    let row = source::object_keys(
        entries
            .iter()
            .map(|(model, at)| {
                Ok((
                    model.as_js_string().clone(),
                    SourceJson::from_serializable(at)?,
                ))
            })
            .collect::<Result<Vec<_>, GatewayError>>()?,
    )?;
    set_member(&mut unsupported, id, row)?;
    set_member(&mut next, "unsupported", unsupported)?;
    Ok(next)
}
fn set_member(value: &mut SourceJson, key: &str, next: SourceJson) -> Result<(), GatewayError> {
    let key = JsString16::from_utf8(key);
    let mut entries =
        source::entries(value).ok_or_else(|| invalid("Source model record is malformed."))?;
    if let Some((_, old)) = entries.iter_mut().find(|(name, _)| name == &key) {
        *old = next;
    } else {
        entries.push((key, next));
    }
    *value = source::object_keys(entries)?;
    Ok(())
}
