//! Source account selectors, generation checks and submitted-workspace projection.
//! Inputs remain data; account mutations stay with their existing authority.
pub mod readback;
use gateway_admin_runtime::claude::{self, AccountBinding, Workspace};
use gateway_auth::types::AuthHost;
use gateway_core::{
    error::GatewayError, js_string::JsString16, source_json::SourceJson,
    source_serialization as source, state::AuthorityStore,
};
pub fn check_target_cohort(
    targeted: bool,
    original_leaf: Option<&SourceJson>,
    actual_cohort: &str,
) -> Result<(), GatewayError> {
    let cohort = original_leaf.and_then(source::string);
    if targeted && cohort.is_none() {
        return Err(gateway_auth::types::invalid(
            "account_cohort_id is required for a targeted clear",
        ));
    }
    if original_leaf.is_some()
        && cohort
            .as_ref()
            .is_none_or(|id| !selector_matches(id, actual_cohort))
    {
        return Err(changed("Claude account generation changed"));
    }
    Ok(())
}

pub fn matching_target<'a>(
    roster: &'a claude::AccountStore,
    id: Option<&JsString16>,
) -> Option<&'a AccountBinding> {
    match id {
        Some(id) => roster
            .accounts
            .iter()
            .find(|account| selector_matches(id, &account.account_id)),
        None => claude::active_binding(roster),
    }
}

fn selector_matches(input: &JsString16, actual_stored_id: &str) -> bool {
    input
        .as_units()
        .iter()
        .copied()
        .eq(actual_stored_id.encode_utf16())
}

pub async fn select_source_account<S: AuthorityStore, H: AuthHost>(
    store: &S,
    platform: &H,
    id: &JsString16,
) -> Result<bool, GatewayError> {
    if let Ok(id) = id.try_utf8() {
        return claude::select_account(store, platform, &id, None).await;
    }
    // Generated stored UUIDs cannot match these units. Preserve the actual
    // existing read/decode error order; do not bootstrap or fabricate an ID.
    if let Some(row) = store.read(&claude::account_store_key()).await? {
        claude::decode_roster_record(&row, platform.now_ms())?;
    }
    Ok(false)
}

pub fn submitted_workspace(
    previous: Option<&Workspace>,
    id: &JsString16,
    requested: bool,
    status: Option<u16>,
    at: i64,
) -> Workspace {
    let previous = previous.filter(|workspace| &workspace.id == id);
    let source = if requested {
        "kv".into()
    } else {
        previous
            .map(|workspace| workspace.source.clone())
            .unwrap_or_else(|| "kv".into())
    };
    Workspace {
        id: id.clone(),
        name: previous.and_then(|workspace| workspace.name.clone()),
        discovered_at_ms: if source == "discovered" {
            Some(at)
        } else {
            previous.and_then(|workspace| workspace.discovered_at_ms)
        },
        verified_at_ms: if status == Some(200) {
            Some(at)
        } else {
            previous.and_then(|workspace| workspace.verified_at_ms)
        },
        source,
        last_error: None,
    }
}

fn changed(message: &str) -> GatewayError {
    GatewayError::new(409, message, Some("claude_code_account_changed"))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn original_cohort_leaf_is_checked_after_lookup_without_presence_loss() {
        let actual = "00000000-0000-0000-0000-000000000002";
        let scalar = SourceJson::from_serializable(actual).unwrap();
        assert!(check_target_cohort(false, None, actual).is_ok());
        assert!(check_target_cohort(true, Some(&scalar), actual).is_ok());
        assert_eq!(
            check_target_cohort(true, None, actual).unwrap_err().status,
            400
        );
        for raw in ["null", "0", "false", "{}"] {
            let leaf = SourceJson::parse(raw).unwrap();
            assert_eq!(
                check_target_cohort(true, Some(&leaf), actual)
                    .unwrap_err()
                    .status,
                400
            );
            assert_eq!(
                check_target_cohort(false, Some(&leaf), actual)
                    .unwrap_err()
                    .status,
                409
            );
        }
        let empty = SourceJson::parse("\"\"").unwrap();
        assert_eq!(
            check_target_cohort(true, Some(&empty), actual)
                .unwrap_err()
                .status,
            409
        );
    }

    #[test]
    fn only_missing_selector_uses_active_account_not_explicit_empty_or_lone_unit()
    -> Result<(), Box<dyn std::error::Error>> {
        let credential = claude::submitted_credential(
            std::sync::Arc::new(claude::source_secret::ClaudeSourceSecret::from_source(
                JsString16::from_utf8("sk-ant-api03-synthetic"),
            )),
            None,
            10,
        )
        .ok_or("fixture credential is missing")?;
        let account = AccountBinding {
            account_id: "00000000-0000-0000-0000-000000000001".into(),
            account_cohort_id: "00000000-0000-0000-0000-000000000002".into(),
            credential_fingerprint: claude::fingerprint(&credential, None),
            evidence_generation: "00000000-0000-0000-0000-000000000003".into(),
            credential,
            workspace: None,
        };
        let roster = claude::AccountStore {
            v: 1,
            active_account_id: Some(account.account_id.clone()),
            accounts: vec![account],
            active_reason: JsString16::from_utf8("admin_selection"),
            updated_at_ms: 10,
        };
        assert!(matching_target(&roster, None).is_some());
        assert!(matching_target(&roster, Some(&JsString16::from_utf8(""))).is_none());
        assert!(matching_target(&roster, Some(&JsString16::from_units(vec![0xd800]))).is_none());
        Ok(())
    }

    #[test]
    fn source_selectors_compare_exact_units_without_new_uuid_input_refusal() {
        let actual = "00000000-0000-0000-0000-000000000001";
        assert!(selector_matches(&JsString16::from_utf8(actual), actual));
        assert!(!selector_matches(
            &JsString16::from_utf8(&format!(" {actual}")),
            actual
        ));
        assert!(!selector_matches(
            &JsString16::from_units(vec![0xd800]),
            actual
        ));
        assert!(!selector_matches(
            &JsString16::from_units(vec![0xdc00]),
            actual
        ));
    }

    #[test]
    fn submitted_workspace_merge_keeps_exact_source_units_without_scalar_projection() {
        let id = JsString16::from_units(vec![119, 114, 107, 115, 112, 99, 95, 0xd800]);
        let name = JsString16::from_units(vec![0xdc00]);
        let previous = Workspace {
            id: id.clone(),
            name: Some(name.clone()),
            source: "kv".into(),
            discovered_at_ms: Some(10),
            verified_at_ms: Some(20),
            last_error: None,
        };
        let merged = submitted_workspace(Some(&previous), &id, false, None, 30);
        assert_eq!(merged.id, id);
        assert_eq!(merged.name, Some(name));
        assert_eq!(merged.verified_at_ms, Some(20));
    }

    #[test]
    fn source_workspace_merge_preserves_or_replaces_generation_timestamps() {
        let previous = Workspace {
            id: "wrkspc_old".into(),
            name: Some("Existing".into()),
            source: "discovered".into(),
            discovered_at_ms: Some(100),
            verified_at_ms: Some(120),
            last_error: Some("old error".into()),
        };
        let inherited = submitted_workspace(
            Some(&previous),
            &JsString16::from_utf8("wrkspc_old"),
            false,
            Some(429),
            500,
        );
        assert_eq!(
            (
                inherited.source.as_str(),
                inherited.discovered_at_ms,
                inherited.verified_at_ms,
                inherited.last_error
            ),
            ("discovered", Some(500), Some(120), None)
        );
        let explicit = submitted_workspace(
            Some(&previous),
            &JsString16::from_utf8("wrkspc_old"),
            true,
            Some(200),
            500,
        );
        assert_eq!(
            (
                explicit.source.as_str(),
                explicit.discovered_at_ms,
                explicit.verified_at_ms
            ),
            ("kv", Some(100), Some(500))
        );
        let replacement = submitted_workspace(
            Some(&previous),
            &JsString16::from_utf8("wrkspc_new"),
            true,
            None,
            500,
        );
        assert_eq!(
            (
                replacement.name,
                replacement.discovered_at_ms,
                replacement.verified_at_ms
            ),
            (None, None, None)
        );
    }
}
