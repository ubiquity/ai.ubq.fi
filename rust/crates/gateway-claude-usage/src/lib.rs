//! Claude subscription usage meters as stored provenance: the source
//! `claude-code-usage-schema.ts` snapshot contract, the retired direct-read
//! `usage` state, and the separate usage-rollup namespace. No meter here is an
//! inference routing guard, and nothing here reads a provider.
pub mod json;
pub mod rollup;
pub mod snapshot;
pub mod state;
