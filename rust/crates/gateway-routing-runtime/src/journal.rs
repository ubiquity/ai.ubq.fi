//! Recoverable request intent only. Counters, pool state and billing remain m03-owned.
use gateway_contracts::identity::RequestContext;
use gateway_routing_journal::reader;
use gateway_routing_journal::record::{Journal, RevisionCheckWire};

use gateway_authority_contracts::TokenSource;
use gateway_core::source_control::encode_source_control_record;
use gateway_core::state::AtomicTransition;
use gateway_core::{state::AuthorityStore, state::CommitOutcome};
use gateway_core::{state::RecordMutation, state::RevisionCheck, state::StoredRecord};
use gateway_kv_codec::key::RecordKey;
use gateway_kv_codec::{tuple_key::ScalarKeyPart, tuple_key::TupleKey};
use gateway_source::error::GatewayError;
use gateway_source::source_json::SourceJson;

pub(crate) fn empty(
    v: u32,
    context: RequestContext,
    input_digest: String,
    lease_expires_at_ms: i64,
    updated_at_ms: i64,
) -> Journal {
    Journal {
        v,
        context,
        input_digest,
        api: None,
        kernel: None,
        cursor: 0,
        attempts: vec![],
        active: None,
        model_sibling_retries: 0,
        refreshed_versions: vec![],
        recovery_attempted: false,
        claude_unavailable: false,
        semantic_committed: false,
        terminal_intent: None,
        finished: false,
        recovering: false,
        lease_expires_at_ms,
        updated_at_ms,
        extras_family: None,
        extras_child_count: None,
        extras_dispatch: None,
        children: vec![],
        kernel_override: None,
        local_work: None,
    }
}

pub fn pending_prefix() -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("routing_runtime"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("pending"),
    ])
    .to_record_key()
}
pub fn pending_key(id: &str) -> RecordKey {
    TupleKey::from_scalar_parts(&[
        ScalarKeyPart::String("uos_ai"),
        ScalarKeyPart::String("routing_runtime"),
        ScalarKeyPart::String("v1"),
        ScalarKeyPart::String("pending"),
        ScalarKeyPart::String(id),
    ])
    .to_record_key()
}
pub fn unavailable(message: &str) -> GatewayError {
    GatewayError::unavailable(message, "routing_session_unavailable")
}
fn mutations<I: TokenSource>(
    journal: &Journal,
    tokens: &I,
) -> Result<Vec<RecordMutation>, GatewayError> {
    let revision = tokens.next_token()?;
    let expires = if journal.finished {
        Some(
            journal
                .context
                .started_at_ms
                .checked_add(gateway_authority_contracts::PAID_RETENTION_MS)
                .ok_or_else(|| unavailable("Routing retention overflow."))?,
        )
    } else {
        None
    };
    if journal.v != 3 {
        journal
            .context
            .principal
            .try_scalar()
            .map_err(|_| unavailable("Routing journal encoding failed."))?;
    }
    let value = SourceJson::from_serializable(journal)
        .map_err(|_| unavailable("Routing journal encoding failed."))?;
    Journal::decode(&value)?;
    let mut rows = vec![RecordMutation::Put {
        key: reader::session_key(&journal.context.request_id.0),
        record: encode_source_control_record(&revision, &value, expires)?,
    }];
    if journal.finished {
        rows.push(RecordMutation::Delete {
            key: pending_key(&journal.context.request_id.0),
        });
    } else {
        rows.push(RecordMutation::Put {
            key: pending_key(&journal.context.request_id.0),
            record: StoredRecord::json(
                format!("{revision}:pending"),
                serde_json::json!({"request_id":journal.context.request_id.0}),
                None,
            ),
        });
    }
    Ok(rows)
}
pub async fn create<S: AuthorityStore, I: TokenSource>(
    store: &S,
    tokens: &I,
    journal: &Journal,
) -> Result<(), GatewayError> {
    let outcome = store
        .commit(AtomicTransition {
            checks: vec![
                RevisionCheck {
                    key: reader::session_key(&journal.context.request_id.0),
                    expected_revision: None,
                },
                RevisionCheck {
                    key: pending_key(&journal.context.request_id.0),
                    expected_revision: None,
                },
            ],
            mutations: mutations(journal, tokens)?,
        })
        .await?;
    if outcome != CommitOutcome::Committed {
        return Err(GatewayError::new(
            409,
            "The request identity is already admitted.",
            Some("request_id_consumed"),
        ));
    }
    Ok(())
}
pub async fn change<
    S: AuthorityStore,
    I: TokenSource,
    F: Fn(&mut Journal) -> Result<(), GatewayError>,
>(
    store: &S,
    tokens: &I,
    id: &str,
    now: i64,
    extra: &[RevisionCheckWire],
    update: F,
) -> Result<Journal, GatewayError> {
    for _ in 0..5 {
        let (old, mut journal) = reader::load(store, id).await?;
        update(&mut journal)?;
        journal.updated_at_ms = now;
        let mut checks = vec![RevisionCheck {
            key: reader::session_key(id),
            expected_revision: Some(old.revision),
        }];
        for extra in extra {
            if let Some(check) = checks.iter().find(|c| c.key == extra.key) {
                if check.expected_revision != extra.expected_revision {
                    return Err(unavailable("The observed source revision changed."));
                }
            } else {
                checks.push(RevisionCheck {
                    key: extra.key.clone(),
                    expected_revision: extra.expected_revision.clone(),
                });
            }
        }
        if store
            .commit(AtomicTransition {
                checks,
                mutations: mutations(&journal, tokens)?,
            })
            .await?
            == CommitOutcome::Committed
        {
            return Ok(journal);
        }
    }
    Err(unavailable("Routing journal changed concurrently."))
}

#[cfg(test)]
mod source_shape_tests {
    use super::*;
    use gateway_local_journal::context::same_source_value;
    #[test]
    fn source_shape_ignores_object_order_not_array_order_or_opaque_identity() {
        let a =
            SourceJson::parse(r#"{"model":"\ud800","items":[1,2],"nested":{"first":1,"last":2}}"#)
                .unwrap();
        let reordered =
            SourceJson::parse(r#"{"nested":{"last":2,"first":1},"items":[1,2],"model":"\ud800"}"#)
                .unwrap();
        assert!(same_source_value(&a, &reordered));
        let array =
            SourceJson::parse(r#"{"model":"\ud800","items":[2,1],"nested":{"first":1,"last":2}}"#)
                .unwrap();
        assert!(!same_source_value(&a, &array));
        let other =
            SourceJson::parse(r#"{"model":"�","items":[1,2],"nested":{"first":1,"last":2}}"#)
                .unwrap();
        assert!(!same_source_value(&a, &other));
    }
}
