//! Source last-write and explicit-check normalization over DATA; never performs a store effect.
use gateway_core::state::{RecordMutation, RevisionCheck};
pub fn normalize_changes(
    checks: Vec<RevisionCheck>,
    mutations: Vec<RecordMutation>,
) -> Option<(Vec<RevisionCheck>, Vec<RecordMutation>)> {
    // Bun KV applies the last mutation for a key. The portable adapter accepts
    // unconditional writes; retain only the source's explicit check conjunction.
    let mut explicit = std::collections::BTreeMap::<String, RevisionCheck>::new();
    for check in checks {
        if let Some(previous) = explicit.get(&check.key.0)
            && previous.expected_revision != check.expected_revision
        {
            return None;
        }
        explicit.insert(check.key.0.clone(), check);
    }
    let mut final_writes = std::collections::BTreeMap::<String, RecordMutation>::new();
    for mutation in mutations {
        let key = match &mutation {
            RecordMutation::Put { key, .. } | RecordMutation::Delete { key } => key,
        };
        final_writes.insert(key.0.clone(), mutation);
    }
    Some((
        explicit.into_values().collect(),
        final_writes.into_values().collect(),
    ))
}
