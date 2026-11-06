//! Request-owned cold OpenRouter resolution, before ordinary provider planning.
use crate::{
    CatalogRuntime,
    host::{CatalogHost, CatalogProvider},
    state::CatalogState,
};
use gateway_core::provider::UpstreamProvider;
use gateway_core::{error::GatewayError, model::ModelId, query::QueryStore};
use gateway_schema::{catalog::CatalogInput, reasoning::js_trim_source};

fn vendor_id(model: &ModelId) -> bool {
    let model = js_trim_source(model.as_js_string());
    let units = model.as_units();
    let whitespace = |unit: &u16| matches!(*unit, 0x0009..=0x000d | 0x0020 | 0x00a0 | 0x1680 | 0x2000..=0x200a | 0x2028 | 0x2029 | 0x202f | 0x205f | 0x3000 | 0xfeff);
    let mut parts = units.split(|unit| *unit == 47);
    !units.iter().any(whitespace)
        && parts.next().is_some_and(|part| !part.is_empty())
        && parts.next().is_some_and(|part| !part.is_empty())
        && parts.next().is_none()
}
fn earlier_direct(input: &CatalogInput, model: &ModelId, responses: bool) -> bool {
    responses && gateway_schema::aliases::is_synthetic(model)
        || [
            ("deepseek", UpstreamProvider::Deepseek),
            ("cerebras", UpstreamProvider::Cerebras),
            ("lithos", UpstreamProvider::Lithos),
        ]
        .iter()
        .any(|(name, provider)| {
            input.enabled(name)
                && gateway_providers::request::canonical_source_model(*provider, model, &[])
                    .is_some()
        })
}
impl CatalogRuntime {
    /// A warm snapshot is authoritative; only a cold vendor/model id refreshes once.
    pub async fn resolve_request_model<S: QueryStore, H: CatalogHost>(
        &self,
        store: &S,
        host: &H,
        input: &CatalogInput,
        model: &ModelId,
        responses: bool,
    ) -> Result<Option<CatalogState>, GatewayError> {
        if !input.enabled("openrouter")
            || !input.configured("openrouter")
            || !vendor_id(model)
            || earlier_direct(input, model, responses)
            || input
                .providers
                .get("openrouter")
                .is_some_and(|row| row.updated_at_ms.is_some())
        {
            return Ok(None);
        }
        if self
            .refresh_provider(store, host, CatalogProvider::Openrouter, false)
            .await?
            .is_none()
        {
            return Err(GatewayError::unavailable(
                "OpenRouter model discovery is unavailable.",
                "openrouter_catalog_unavailable",
            ));
        }
        let publication = host.publication().await?;
        self.load_state(store, &publication, host.now_ms())
            .await
            .map(Some)
    }
}
