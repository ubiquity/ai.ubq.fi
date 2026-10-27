use crate::capabilities::*;
use gateway_admin_contracts::fields::{
    boolean, bounded_positive, cursor, digits, first, truth, uuid,
};
use gateway_admin_contracts::{operations::AdminInput, source_input as source_body};
use gateway_auth::types::invalid;
use gateway_core::{error::GatewayError, identity::MAX_SAFE_INTEGER, source_json::SourceJson};
use gateway_profile_values::coercion::js_number;
use serde_json::Value;
use std::collections::BTreeSet;

fn incident_id(value: &str) -> bool {
    value.strip_prefix("provider-").is_some_and(|id| {
        uuid(id)
            && id
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b) || b == b'-')
            && id.as_bytes()[14] == b'4'
            && b"89ab".contains(&id.as_bytes()[19])
    })
}

pub fn capture(input: &AdminInput, incidents: bool) -> Result<CaptureRequest, GatewayError> {
    let q = &input.query;
    if incidents {
        if q.keys()
            .any(|k| !["limit", "cursor", "incident_id"].contains(&k.as_str()))
            || q.values().any(|v| v.len() != 1)
        {
            return Err(invalid("Invalid incident query"));
        }
        let limit = bounded_positive(first(q, "limit"), 20, 100)?;
        if first(q, "limit").is_some_and(|s| !digits(s) || s.parse::<u64>().is_ok_and(|v| v > 100))
        {
            return Err(invalid("limit must be an integer from 1 to 100"));
        }
        let id = first(q, "incident_id").map(str::to_owned);
        if id.as_ref().is_some_and(|s| !incident_id(s)) {
            return Err(invalid("Invalid incident id"));
        }
        return Ok(CaptureRequest::Incidents {
            incident_id: id,
            cursor: cursor(first(q, "cursor"), false)?,
            limit,
        });
    }
    let incident = first(q, "incident_id")
        .map(gateway_schema::util::js_trim)
        .filter(|s| !s.is_empty());
    let request = first(q, "request_id")
        .map(gateway_schema::util::js_trim)
        .filter(|s| !s.is_empty());
    if let Some(request) = request {
        if incident.is_some()
            || request.len() > 128
            || request.is_empty()
            || !request
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b))
        {
            return Err(invalid("Invalid replay request id"));
        }
        return Ok(CaptureRequest::Request {
            request_id: request.into(),
        });
    }
    let number = |name, fallback| {
        first(q, name)
            .filter(|s| !s.is_empty())
            .map(|s| {
                let n = js_number(&Value::String(s.into()));
                (n.is_finite() && n.fract() == 0.0 && n >= 0.0 && n <= MAX_SAFE_INTEGER as f64)
                    .then_some(n as i64)
            })
            .unwrap_or(Some(fallback))
    };
    let after = number("after_ms", 0).ok_or_else(|| invalid("Invalid replay interval"))?;
    let before = number("before_ms", -1).ok_or_else(|| invalid("Invalid replay interval"))?;
    if before < after || number("limit", 1) != Some(1) {
        return Err(invalid("Invalid replay interval or limit"));
    }
    let cursor = cursor(first(q, "cursor"), true)?;
    if let Some(id) = incident {
        if !incident_id(id) {
            return Err(invalid("Invalid incident id"));
        }
        Ok(CaptureRequest::Incident {
            incident_id: id.into(),
            cursor,
        })
    } else {
        Ok(CaptureRequest::Interval {
            after_ms: after,
            before_ms: before,
            cursor,
        })
    }
}

pub fn migration(input: &AdminInput) -> Result<MigrationImport, GatewayError> {
    let ndjson: String = input.body.deserialize().map_err(|_| {
        GatewayError::unavailable(
            "Migration import requires the preserved raw NDJSON body",
            "admin_raw_body_unavailable",
        )
    })?;
    if ndjson.len() > 5 * 1024 * 1024 {
        return Err(GatewayError::new(
            413,
            "Migration body is too large",
            Some("invalid_request_error"),
        ));
    }
    let q = &input.query;
    let profile = match first(q, "profile")
        .unwrap_or("")
        .trim_matches(gateway_schema::util::js_whitespace)
    {
        "" | "prod" => MigrationProfile::Prod,
        "local" => MigrationProfile::Local,
        _ => return Err(invalid("profile must be local or prod")),
    };
    let write = boolean(first(q, "write")) == Some(true);
    if write && boolean(first(q, "dry_run")) == Some(true) {
        return Err(invalid("dry_run and write are mutually exclusive"));
    }
    Ok(MigrationImport {
        ndjson,
        profile,
        include_cache: truth(first(q, "include_cache")),
        include_legacy: boolean(first(q, "include_legacy"))
            .unwrap_or(profile == MigrationProfile::Local),
        overwrite: truth(first(q, "overwrite")),
        dry_run: !write,
    })
}

pub fn claude(body: &Value) -> Result<ClaudeSubmission, GatewayError> {
    // Compatibility only for callers that actually own a scalar Value.
    claude_source(&SourceJson::from_serializable(body)?)
}
pub fn claude_source(body: &SourceJson) -> Result<ClaudeSubmission, GatewayError> {
    let credential = source_body::member(body, "credential")
        .as_ref()
        .and_then(source_body::text)
        .or_else(|| {
            source_body::member(body, "api_key")
                .as_ref()
                .and_then(source_body::text)
        })
        .map(|text| source_body::trim(&text));
    if credential
        .as_ref()
        .is_some_and(|text| text.as_units().is_empty())
    {
        return Err(invalid("credential must not be blank"));
    }
    if credential
        .as_ref()
        .is_some_and(|text| text.as_units().len() > 512)
    {
        return Err(invalid("credential is too long"));
    }
    if credential.as_ref().is_some_and(|text| {
        !text
            .as_units()
            .starts_with(&"sk-ant-".encode_utf16().collect::<Vec<_>>())
    }) {
        return Err(invalid(
            "credential must be an Anthropic API key or Claude Code OAuth token",
        ));
    }
    let label = source_body::member(body, "label")
        .as_ref()
        .and_then(source_body::text)
        .map(|text| source_body::trim(&text));
    if label
        .as_ref()
        .is_some_and(|text| text.as_units().len() > 64)
    {
        return Err(invalid("label is too long"));
    }
    let workspace_id = match source_body::member(body, "workspace_id") {
        None => None,
        Some(value) if value.as_str() == "null" => Some(None),
        Some(value) => {
            let text = source_body::text(&value)
                .ok_or_else(|| invalid("workspace_id must be a wrkspc_ workspace id"))?;
            if text.as_units().is_empty() {
                Some(None)
            } else {
                let text = source_body::trim(&text);
                if text.as_units().len() > 256
                    || !text
                        .as_units()
                        .starts_with(&"wrkspc_".encode_utf16().collect::<Vec<_>>())
                {
                    return Err(invalid("workspace_id must be a wrkspc_ workspace id"));
                }
                Some(Some(text))
            }
        }
    };
    if credential.is_none() && workspace_id.is_none() {
        return Err(invalid("credential or workspace_id is required"));
    }
    let account_id = match source_body::member(body, "account_id") {
        None => None,
        Some(value) => {
            let id = source_body::text(&value)
                .filter(|text| {
                    text.as_units().len() == 36
                        && text.as_units().iter().all(|unit| {
                            (48..=57).contains(unit) || (97..=102).contains(unit) || *unit == 45
                        })
                })
                .ok_or_else(|| invalid("account_id is invalid"))?;
            id.try_utf8()
                .map(Some)
                .map_err(|_| invalid("account_id is invalid"))?
        }
    };
    for name in ["create_new", "activate"] {
        if source_body::member(body, name)
            .as_ref()
            .is_some_and(|value| source_body::boolean(value).is_none())
        {
            return Err(invalid(&format!("{name} must be boolean")));
        }
    }
    let create_new = source_body::member(body, "create_new")
        .as_ref()
        .and_then(source_body::boolean)
        == Some(true);
    if create_new && account_id.is_some() {
        return Err(invalid("create_new cannot target an existing account"));
    }
    Ok(ClaudeSubmission {
        credential: credential.map(|text| {
            std::sync::Arc::new(crate::claude::source_secret::ClaudeSourceSecret::from_source(text))
        }),
        label,
        workspace_id,
        account_id,
        create_new,
        activate: source_body::member(body, "activate")
            .as_ref()
            .and_then(source_body::boolean)
            == Some(true),
    })
}

pub fn prompt_groups(input: &AdminInput) -> Result<Vec<String>, GatewayError> {
    if input.query.keys().any(|k| k != "group_by") {
        return Err(invalid(
            "Only group_by is supported for prompt-cache analytics",
        ));
    }
    if input.query.get("group_by").is_some_and(|v| v.len() > 1) {
        return Err(invalid("group_by may appear only once"));
    }
    let groups: Vec<String> = first(&input.query, "group_by")
        .unwrap_or("key_presence")
        .split(',')
        .map(|s| s.trim_matches(gateway_schema::util::js_whitespace).into())
        .collect();
    let allowed = [
        "provider",
        "model",
        "route",
        "key_presence",
        "mode",
        "fallback",
    ];
    if groups.is_empty()
        || groups.len() > 2
        || groups.iter().any(|s| !allowed.contains(&s.as_str()))
        || groups.iter().collect::<BTreeSet<_>>().len() != groups.len()
    {
        return Err(invalid(
            "group_by must contain up to two distinct values from provider, model, route, key_presence, mode, or fallback",
        ));
    }
    Ok(groups)
}
