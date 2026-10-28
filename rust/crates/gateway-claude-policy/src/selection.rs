use gateway_claude_roster::claude::{self, RosterRead};
// Pure refusal wire and revision-check DATA, never a checked currentness capability.
use gateway_core::state::RevisionCheck;
use gateway_source::{error::GatewayError, js_string::JsString16, source_json::SourceJson};
use serde::Serialize;
use std::collections::BTreeMap;
pub fn cooldown_body(message: &JsString16) -> Result<SourceJson, GatewayError> {
    #[derive(Serialize)]
    struct Error<'a> {
        message: &'a JsString16,
        r#type: &'static str,
        code: &'static str,
    }
    #[derive(Serialize)]
    struct Envelope<'a> {
        error: Error<'a>,
    }
    SourceJson::from_serializable(&Envelope {
        error: Error {
            message,
            r#type: "invalid_request_error",
            code: "claude_code_cooling_down",
        },
    })
}
pub fn deduplicate(checks: Vec<RevisionCheck>) -> Result<Vec<RevisionCheck>, GatewayError> {
    let mut map = BTreeMap::new();
    for check in checks {
        if let Some(old) = map.insert(check.key.0, check.expected_revision.clone())
            && old != check.expected_revision
        {
            return Err(crate::error::invalid("Claude source checks disagree."));
        }
    }
    Ok(map
        .into_iter()
        .map(|(key, expected_revision)| RevisionCheck {
            key: gateway_kv_codec::key::RecordKey(key),
            expected_revision,
        })
        .collect())
}

pub fn same_pool(left: &RosterRead, right: &RosterRead) -> bool {
    left.roster.active_account_id == right.roster.active_account_id
        && left.roster.accounts.len() == right.roster.accounts.len()
        && left
            .roster
            .accounts
            .iter()
            .zip(&right.roster.accounts)
            .all(|(a, b)| claude::same_binding(a, b))
}
