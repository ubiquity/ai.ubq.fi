//! Source-derived OpenAI/Codex request and catalog domain. No credentials or provider I/O.
pub mod catalog;
pub mod catalog_cache;
pub mod catalog_store;
pub mod catalog_views;
pub mod metadata;
pub(crate) mod model_control;
pub mod native_catalog;
pub mod provider_catalog;
pub mod reasoning;
pub mod runtime_config;
pub mod snapshot;
