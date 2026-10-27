use gateway_core::source_json::SourceJson;
use gateway_provider_protocol::types::ClientSurface;
use gateway_routing_runtime::policy;
use gateway_routing_runtime_test_support::{
    input::{NOW, input},
    source_catalog::Collator,
};
use gateway_schema::selection::ProviderSelection;
use serde_json::json;
#[test]
fn decision_models_cannot_enter_either_text_wire() -> Result<(), Box<dyn std::error::Error>> {
    for surface in [ClientSurface::Responses, ClientSurface::Chat] {
        let mut request = input(
            "decision-text-reject",
            "baseten/elephant-1:free",
            &["openrouter"],
        );
        request.surface = surface;
        request.raw = SourceJson::from_serializable(&if surface == ClientSurface::Chat {
            json!({"model":"baseten/elephant-1:free","messages":[{"role":"user","content":"fixture"}]})
        } else {
            json!({"model":"baseten/elephant-1:free","input":"fixture"})
        })?;
        request.catalog.selection = ProviderSelection::normalize(
            &json!({"provider_ids":["openrouter"],"updated_at_ms":NOW}),
        );
        request
            .catalog
            .providers
            .get_mut("openrouter")
            .ok_or("missing fixture provider")?
            .models = vec![SourceJson::from_serializable(
            &json!({"id":"baseten/elephant-1:free","output_modalities":["decisions"]}),
        )?];
        let result = policy::plan(&request, NOW, &Collator::captured());
        assert!(result.is_err(), "decision-only model entered a text plan");
    }
    Ok(())
}
