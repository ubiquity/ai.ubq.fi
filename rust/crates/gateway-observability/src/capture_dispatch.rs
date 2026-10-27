//! Original authenticated capture request dispatch; defining operations retain all effects.
use crate::ObservabilityRuntime;
use gateway_admin_runtime::{capabilities::CaptureRequest, capabilities::Context};
use gateway_auth_contracts::types::AuthResponse;
use gateway_core::query::QueryStore;
use gateway_observability_contracts::types::ObservabilityHost;
use gateway_source::error::GatewayError;
impl<S:QueryStore,H:ObservabilityHost> ObservabilityRuntime<S,H> {
    pub async fn capture(
        &self,
        request: CaptureRequest,
        _context: Context,
    ) -> Result<AuthResponse, GatewayError> {
        match request {
            CaptureRequest::Request { request_id } => {
                self.exports().export_capture_request(&request_id).await
            }
            CaptureRequest::Interval {
                after_ms,
                before_ms,
                cursor,
            } => {
                self.exports().export_capture_interval(after_ms, before_ms, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incident {
                incident_id,
                cursor,
            } => {
                self.exports().export_incident_captures(&incident_id, cursor.as_deref())
                    .await
            }
            CaptureRequest::Incidents {
                incident_id,
                cursor,
                limit,
            } => {
                self.incidents().list_incidents(incident_id.as_deref(), cursor.as_deref(), limit)
                    .await
            }
        }
    }
}
