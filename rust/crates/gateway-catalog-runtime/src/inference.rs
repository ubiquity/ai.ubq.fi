//! Complete cold model discovery before the request freezes its routing input.
use crate::CatalogRuntime;
use gateway_catalog_inputs::model::uses_openrouter;
use gateway_catalog_provider::host::{CatalogHost, CatalogProvider};
use gateway_contracts::model::ModelId;
use gateway_core::query::QueryStore;
use gateway_request_normalization::document::SourceRequestDocument;
use gateway_schema_values::{request::SourceModelRequest, source_view::js_trim_source};
use gateway_source::error::GatewayError;

impl CatalogRuntime {
    /// A warm snapshot, including an empty one, remains authoritative. Cold
    /// requests join the existing credential-bound refresh rather than inventing
    /// an allowed model or freezing the background refresh's earlier empty input.
    pub async fn resolve_inference_catalog<S: QueryStore, H: CatalogHost>(
        &self,
        store: &S,
        host: &H,
        input: &mut gateway_catalog_input::catalog::CatalogInput,
        document: &SourceRequestDocument,
    ) -> Result<(), GatewayError> {
        let Ok(SourceModelRequest::Named(name)) = document.model_request() else {
            return Ok(());
        };
        let model = ModelId::from_source(js_trim_source(&name));
        if input
            .providers
            .get("openrouter")
            .is_some_and(|row| row.updated_at_ms.is_some())
            || !uses_openrouter(input, &model)
        {
            return Ok(());
        }
        self.refresh_provider(store, host, CatalogProvider::Openrouter, false)
            .await?;
        let publication = host.publication().await?;
        if let Some(snapshot) = self.cached_provider(&publication, CatalogProvider::Openrouter) {
            input.providers.insert("openrouter".into(), snapshot);
        }
        Ok(())
    }
}
