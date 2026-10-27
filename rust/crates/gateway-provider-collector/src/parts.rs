//! Ordered text positions and item aliases mirror the source stream's Map order.
use gateway_core::{error::GatewayError, js_string::JsString16, source_json::SourceJson, source_serialization as json};
use gateway_provider_source::source as s;
use gateway_provider_source::extras::malformed;

#[derive(Clone, PartialEq)]
enum Key { Output(JsString16, JsString16), Item(JsString16, JsString16) }

struct Identity { position: Option<JsString16>, item: Option<JsString16>, content: JsString16 }
impl Identity {
    fn event(event: &SourceJson) -> Result<Self, GatewayError> {
        Ok(Self { position: index(s::present(event, "output_index"))?, item: item_id(event, "item_id"), content: index(s::present(event, "content_index"))?.unwrap_or_else(|| "0".into()) })
    }
    fn key(&self) -> Key {
        match &self.position {
            Some(position) => Key::Output(position.clone(), self.content.clone()),
            None => self.item.as_ref().map_or_else(|| Key::Output("0".into(), self.content.clone()), |item| Key::Item(item.clone(), self.content.clone())),
        }
    }
}
fn item_id(value: &SourceJson, name: &str) -> Option<JsString16> {
    s::present(value, name).and_then(|value| s::string(&value)).map(|value| s::trim(&value)).filter(|value| !value.as_units().is_empty())
}
fn index(value: Option<SourceJson>) -> Result<Option<JsString16>, GatewayError> {
    let Some(value) = value else { return Ok(None); };
    if let Some(text) = s::string(&value) { return Ok(Some(text)); }
    if let Some(number) = json::number(&value) {
        if number.is_infinite() { return Ok(Some(JsString16::from_utf8(if number.is_sign_negative() { "-Infinity" } else { "Infinity" }))); }
        return Ok(Some(s::canonical(&value)?.as_str().into()));
    }
    Ok(None)
}

#[derive(Clone, Default)]
pub(crate) struct Parts { rows: Vec<(Key, JsString16)> }
impl Parts {
    fn get(&self, key: &Key) -> Option<&JsString16> { self.rows.iter().find(|(entry, _)| entry == key).map(|(_, value)| value) }
    fn set(&mut self, key: Key, value: JsString16) {
        if let Some((_, old)) = self.rows.iter_mut().find(|(entry, _)| entry == &key) { *old = value; }
        else { self.rows.push((key, value)); }
    }
    fn store(&mut self, identity: &Identity, value: JsString16) {
        self.set(identity.key(), value.clone());
        if identity.position.is_some() && let Some(item) = &identity.item { self.set(Key::Item(item.clone(), identity.content.clone()), value); }
    }
    pub(crate) fn is_empty(&self) -> bool { self.rows.iter().all(|(_, value)| value.as_units().is_empty()) }
    pub(crate) fn delta(&mut self, event: &SourceJson, delta: &JsString16) -> Result<(), GatewayError> {
        let identity = Identity::event(event)?;
        let old = self.get(&identity.key()).cloned().unwrap_or_default();
        self.store(&identity, s::concat(&[&old, delta]));
        Ok(())
    }
    fn repeated(&self, identity: &Identity, completed: &JsString16) -> Option<JsString16> {
        if completed.as_units().is_empty() || identity.item.is_some() { return None; }
        let matches = |value: &JsString16| value.as_units().starts_with(completed.as_units());
        if identity.position.is_some() && let Some(old) = self.get(&identity.key()).filter(|value| matches(value)) { return Some(old.clone()); }
        let item_rows = self.rows.iter().filter(|(key, _)| matches!(key, Key::Item(..))).collect::<Vec<_>>();
        if let [(_, value)] = item_rows.as_slice() && matches(value) { return Some(value.clone()); }
        let positions = self.rows.iter().filter(|(key, _)| matches!(key, Key::Output(..))).collect::<Vec<_>>();
        let rows = if positions.is_empty() { self.rows.iter().collect::<Vec<_>>() } else { positions };
        if rows.len() < 2 { return None; }
        let aggregate = s::join(&rows.iter().map(|(_, value)| value.clone()).collect::<Vec<_>>(), "");
        (aggregate == *completed).then_some(aggregate)
    }
    fn reconcile(&mut self, identity: Identity, full: &JsString16) -> Result<JsString16, GatewayError> {
        if let Some(repeated) = self.repeated(&identity, full) { self.store(&identity, repeated); return Ok(JsString16::default()); }
        let old = self.get(&identity.key()).cloned().unwrap_or_default();
        let delta = if full.as_units().is_empty() || old.as_units().starts_with(full.as_units()) { JsString16::default() }
        else if let Some(suffix) = full.as_units().strip_prefix(old.as_units()) { JsString16::from_units(suffix.to_vec()) }
        else { return Err(malformed("Completed output disagrees with streamed prefix.")); };
        self.store(&identity, s::concat(&[&old, &delta]));
        Ok(delta)
    }
    pub(crate) fn done(&mut self, event: &SourceJson, full: &JsString16) -> Result<JsString16, GatewayError> { self.reconcile(Identity::event(event)?, full) }
    pub(crate) fn completed(&mut self, output: usize, content: usize, item: &SourceJson, full: &JsString16) -> Result<JsString16, GatewayError> {
        self.reconcile(Identity { position: Some(JsString16::from_utf8(&output.to_string())), item: item_id(item, "id"), content: JsString16::from_utf8(&content.to_string()) }, full)
    }
}
