use gateway_contracts::model::ModelId;
use gateway_contracts::provider::UpstreamProvider;
use gateway_local_refusal::source_error::SourceRequestError;
use gateway_provider_policy::request::canonical_source_model;
use gateway_provider_protocol::types::ClientSurface;
use gateway_provider_source::source as s;
use gateway_schema::source_request::SourceRequestDocument;
use gateway_schema_values::request::RequestSurface;
use gateway_schema_values::request::SourceModelRequest;
use gateway_source::js_string::JsString16;
use gateway_source::source_json::SourceJson;

pub fn source_prelude(
    provider: UpstreamProvider,
    surface: ClientSurface,
    raw: &SourceJson,
    selected_model: &ModelId,
    allowed_models: &[ModelId],
) -> s::Result<(SourceRequestDocument, bool, ModelId, ModelId)> {
    let document = SourceRequestDocument::new(raw.clone())?;
    let schema_surface = if surface == ClientSurface::Chat {
        RequestSurface::ChatCompletions
    } else {
        RequestSurface::Responses
    };
    let view = document.for_surface(schema_surface);
    // The caller owns the common envelope phase; selected-provider preparation must not repeat it.
    let native_openrouter = provider == UpstreamProvider::Openrouter;
    // OpenRouter's native wire uses `=== true`, not ordinary parse_stream.
    let client_stream = if native_openrouter {
        s::present(raw, "stream").and_then(|observed| s::boolean(&observed)) == Some(true)
    } else if surface == ClientSurface::Responses {
        view.stream()?
    } else {
        false
    };
    let requested = match view.model_request()? {
        SourceModelRequest::Default => selected_model.clone(),
        SourceModelRequest::Named(name) => ModelId::from_source(name),
    };
    if surface == ClientSurface::Chat {
        view.max_completion_tokens()?;
    }
    let model =
        canonical_source_model(provider, selected_model, allowed_models).ok_or_else(|| {
            SourceRequestError::new(
                404,
                s::concat(&[
                    &JsString16::from_utf8("The model '"),
                    requested.as_js_string(),
                    &JsString16::from_utf8("' does not exist or is not available."),
                ]),
                "invalid_request_error",
                Some("model_not_found"),
                None,
            )
        })?;
    Ok((document, client_stream, requested, model))
}
