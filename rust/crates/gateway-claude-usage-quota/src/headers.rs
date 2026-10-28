//! Header-only meter DATA. This does not establish account or routing authority.
use crate::{QuotaObservation, QuotaWindow, STANDARD_FAMILIES, StandardWindow, WINDOWS};
use gateway_source::source_json::SourceJson;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub enum HeaderSource {
    #[serde(rename = "response_headers")]
    ResponseHeaders,
}
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct HeaderObservation {
    pub source: HeaderSource,
    pub windows: BTreeMap<String, QuotaWindow>,
    pub standard: BTreeMap<String, StandardWindow>,
    pub representative_claim: Option<String>,
    pub observed_at_ms: f64,
}

pub fn read(value: &Value) -> Option<HeaderObservation> {
    if value.get("source")?.as_str()? != "response_headers" || value.get("usage_snapshot").is_some()
    {
        return None;
    }
    let at = crate::number(value.get("observed_at_ms"))
        .filter(|at| crate::safe_integer(*at) && *at > 0.0)?;
    Some(HeaderObservation {
        source: HeaderSource::ResponseHeaders,
        windows: WINDOWS
            .into_iter()
            .filter_map(|id| crate::window(&value["windows"][id]).map(|row| (id.into(), row)))
            .collect(),
        standard: STANDARD_FAMILIES
            .into_iter()
            .filter_map(|id| crate::standard(&value["standard"][id]).map(|row| (id.into(), row)))
            .collect(),
        representative_claim: value
            .get("representative_claim")
            .and_then(Value::as_str)
            .map(str::to_owned),
        observed_at_ms: at,
    })
}
pub fn read_source(value: &SourceJson) -> Option<HeaderObservation> {
    read(&value.deserialize::<Value>().ok()?)
}
pub fn deserialize_optional<'de, D: serde::Deserializer<'de>>(
    deserializer: D,
) -> Result<Option<HeaderObservation>, D::Error> {
    Value::deserialize(deserializer).map(|value| read(&value))
}

pub fn merge(
    previous: Option<&HeaderObservation>,
    observation: Option<&QuotaObservation>,
) -> Option<HeaderObservation> {
    let Some(observation) = observation.filter(|o| o.usage_snapshot.is_none() && !o.is_empty())
    else {
        return previous.cloned();
    };
    let mut next = previous.cloned().unwrap_or(HeaderObservation {
        source: HeaderSource::ResponseHeaders,
        windows: BTreeMap::new(),
        standard: BTreeMap::new(),
        representative_claim: None,
        observed_at_ms: observation.observed_at_ms,
    });
    for id in WINDOWS {
        if let Some(row) = observation.windows.get(id)
            && next
                .windows
                .get(id)
                .is_none_or(|old| old.observed_at_ms <= row.observed_at_ms)
        {
            next.windows.insert(id.into(), row.clone());
        }
    }
    for id in STANDARD_FAMILIES {
        if let Some(row) = observation.standard.get(id)
            && next
                .standard
                .get(id)
                .is_none_or(|old| old.observed_at_ms <= row.observed_at_ms)
        {
            next.standard.insert(id.into(), row.clone());
        }
    }
    next.representative_claim = observation
        .representative_claim
        .clone()
        .or(next.representative_claim);
    next.observed_at_ms = next.observed_at_ms.max(observation.observed_at_ms);
    Some(next)
}

#[cfg(test)]
mod tests {
    use crate::headers::{merge, read};
    use crate::{QuotaObservation, QuotaWindow};
    use serde_json::json;
    #[test]
    fn unmarked_or_direct_data_is_not_header_evidence() {
        let raw =
            json!({"source":"response_headers","observed_at_ms":1000,"windows":{},"standard":{}});
        assert!(read(&raw).is_some());
        for patch in [
            json!({}),
            json!({"source":"oauth_usage","observed_at_ms":1000}),
            json!({"source":"response_headers","observed_at_ms":0}),
            json!({"source":"response_headers","observed_at_ms":1000,"usage_snapshot":null}),
        ] {
            assert!(read(&patch).is_none());
        }
    }
    #[test]
    fn each_family_retains_its_own_clock_and_headerless_reads_do_not_renew_it() {
        let mut first = QuotaObservation::empty(1000);
        first.windows.insert(
            "5h".into(),
            QuotaWindow {
                status: None,
                utilization: Some(0.4),
                reset_at_ms: None,
                observed_at_ms: 1000.0,
            },
        );
        let initial = merge(None, Some(&first)).unwrap();
        let mut partial = QuotaObservation::empty(900);
        partial.windows.insert(
            "5h".into(),
            QuotaWindow {
                utilization: Some(0.1),
                ..first.windows["5h"].clone()
            },
        );
        partial.windows.get_mut("5h").unwrap().observed_at_ms = 900.0;
        partial.windows.insert(
            "7d".into(),
            QuotaWindow {
                status: None,
                utilization: Some(0.2),
                reset_at_ms: None,
                observed_at_ms: 900.0,
            },
        );
        let next = merge(Some(&initial), Some(&partial)).unwrap();
        assert_eq!(next.windows["5h"].utilization, Some(0.4));
        assert_eq!(next.windows["7d"].observed_at_ms, 900.0);
        assert_eq!(
            merge(Some(&next), Some(&QuotaObservation::empty(2000))),
            Some(next)
        );
    }
}
