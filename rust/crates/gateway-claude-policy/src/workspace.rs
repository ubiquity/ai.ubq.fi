//! Fixed workspace response projection only; effects and currentness remain execution-owned.
use gateway_schema::reasoning::js_trim_source;
use gateway_source::{
    js_string::JsString16, js_string_case::lowercase_source, source_json::SourceJson,
    source_serialization as source,
};
pub fn target(body: &str) -> Option<(JsString16, Option<JsString16>)> {
    let parsed = SourceJson::parse(body).ok();
    let mut workspaces = parsed
        .as_ref()
        .and_then(|value| source::member(value, "data"))
        .as_ref()
        .and_then(source::array)
        .into_iter()
        .flatten()
        .filter_map(|row| {
            let id = js_trim_source(&source::string(&source::member(&row, "id")?)?);
            if !id
                .as_units()
                .iter()
                .copied()
                .take(7)
                .eq("wrkspc_".encode_utf16())
                || source::member(&row, "archived_at").is_some_and(|v| v.as_str() != "null")
            {
                return None;
            }
            let name = source::member(&row, "name")
                .as_ref()
                .and_then(source::string)
                .map(|name| js_trim_source(&name))
                .filter(|name| !name.as_units().is_empty());
            Some((id, name))
        })
        .collect::<Vec<_>>();
    let index = workspaces
        .iter()
        .position(|(_, name)| {
            name.as_ref()
                .is_some_and(|name| lowercase_source(name) == JsString16::from_utf8("default"))
        })
        .unwrap_or(0);
    if workspaces.is_empty() {
        return None;
    }
    Some(workspaces.remove(index))
}
