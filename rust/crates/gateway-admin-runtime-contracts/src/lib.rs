//! Finite administrative host contracts and source input parsing.
pub mod capabilities;
pub mod parse;

#[derive(Clone, Debug, Default)]
pub struct RuntimeConfig {
    /// Explicit existing source configuration passed at bootstrap, never discovered by this crate.
    pub claude_allowed_key_ids: Vec<String>,
}
