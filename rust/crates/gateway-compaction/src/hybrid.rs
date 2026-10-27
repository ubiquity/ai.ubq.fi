//! Buffered concurrent residue effect driver; host and request lifetime remain caller-owned.
use futures_util::{StreamExt, stream};
use gateway_compaction_render::hybrid::{chunks, compose, residue_body, residue_text};
use gateway_compaction_source::{
    text::{self, Text},
    types::CompactionHost,
};
pub(super) async fn run<H: CompactionHost>(
    pure: &Text,
    host: &H,
    max: usize,
    timeout_ms: u32,
) -> Option<Text> {
    let chunks = chunks(pure, max);
    let results = stream::iter(chunks.iter().enumerate().map(|(i, chunk)| async move {
        let result = match residue_body(chunk) {
            Ok(body) => host
                .rewrite_residue(&body, timeout_ms)
                .await
                .ok()
                .and_then(|r| residue_text(&r)),
            Err(_) => None,
        };
        (i, result)
    }))
    .buffer_unordered(4)
    .collect::<Vec<_>>()
    .await;
    if results.iter().all(|(_, result)| result.is_none()) {
        return None;
    }
    let mut texts = chunks;
    for (i, result) in results {
        if let Some(result) = result {
            texts[i] = result;
        }
    }
    let result = compose(&text::join(&texts, "\n\n"), pure);
    (!result.blank() && result.len() < pure.len()).then_some(result)
}
