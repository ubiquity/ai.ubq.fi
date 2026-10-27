//! Fixed OpenLux metadata reads retain their original request and cancellation phase.
use super::{Binding, unavailable};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use std::collections::BTreeMap;

pub(super) enum Metadata {
    Ratios,
    Status,
}
impl Metadata {
    fn url(&self) -> &'static str {
        match self {
            Self::Ratios => "https://api.openlux.ai/api/ratio_config",
            Self::Status => "https://api.openlux.ai/api/status",
        }
    }
}
pub(super) async fn fetch(host: &Binding, endpoint: Metadata) -> Result<SourceJson, GatewayError> {
    let target = crate::transport::outbound::PinnedDestination::new(endpoint.url())?;
    let cancellation = host.catalog.platform.cancellation.child()?;
    let headers = BTreeMap::from([("Accept".into(), "application/json".into())]);
    let mut response = crate::transport::outbound::send_http(
        &target,
        worker::Method::Get,
        &headers,
        &[],
        &cancellation,
        || async { Ok(()) },
    )
    .await
    .map_err(|failure| failure.error)?;
    if !(200..300).contains(&response.status_code()) {
        gateway_worker_http_body::body::discard_response(&mut response);
        return Err(unavailable(
            "Administrator metadata or summarizer request failed.",
        ));
    }
    let bytes = response
        .bytes()
        .await
        .map_err(|_| unavailable("Administrator response could not be read."))?;
    gateway_worker_web::admin_pricing::parse_metadata(&bytes)
}
