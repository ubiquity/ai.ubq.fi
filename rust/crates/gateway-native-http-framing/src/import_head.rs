//! Import route/header framing DATA only; no authenticated admission helpers.
use gateway_auth_native_contracts::native_import::NativeImportKind;
use std::time::Instant;

pub fn kind(path: &str) -> Option<NativeImportKind> {
    [NativeImportKind::CodexAuth, NativeImportKind::NativeProfile]
        .into_iter()
        .find(|kind| {
            path == kind.path()
                || (*kind == NativeImportKind::NativeProfile
                    && path
                        .strip_prefix(kind.path())
                        .is_some_and(|suffix| suffix.starts_with('?')))
        })
}
pub struct ImportHead {
    pub kind: NativeImportKind,
    pub path: String,
    pub headers: Vec<(String, String)>,
    pub framing_deadline: Instant,
}
