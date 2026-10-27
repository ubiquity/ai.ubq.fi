use gateway_claude_roster::claude::{
    AccountBinding, AccountStore, Credential, Workspace, fingerprint,
    source_secret::ClaudeSourceSecret, submitted_credential,
};
use gateway_source::js_string::JsString16;
use std::sync::Arc;
pub fn units(values: &[u16]) -> JsString16 {
    JsString16::from_units(values.to_vec())
}
pub fn credential(secret: JsString16) -> Credential {
    submitted_credential(
        Arc::new(ClaudeSourceSecret::from_source(secret)),
        Some(units(&[0xdc00])),
        100,
    )
    .unwrap()
}
pub fn workspace(id: JsString16) -> Workspace {
    Workspace {
        id,
        name: Some(units(&[0xd800])),
        source: "kv".into(),
        discovered_at_ms: None,
        verified_at_ms: None,
        last_error: None,
    }
}
pub fn roster(credential: Credential, workspace: Option<Workspace>) -> AccountStore {
    let fingerprint = fingerprint(&credential, workspace.as_ref());
    AccountStore {
        v: 1,
        accounts: vec![AccountBinding {
            account_id: "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa".into(),
            account_cohort_id: "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb".into(),
            evidence_generation: "cccccccc-cccc-cccc-cccc-cccccccccccc".into(),
            credential_fingerprint: fingerprint,
            credential,
            workspace,
        }],
        active_account_id: Some("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa".into()),
        active_reason: JsString16::from_utf8("admin_selection"),
        updated_at_ms: 100,
    }
}
