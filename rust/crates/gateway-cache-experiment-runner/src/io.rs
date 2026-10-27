//! Original fixed awaited cache experiment host interface and source request body.
use gateway_cache_experiment_dispatch::{CacheDispatch, RefreshPermit};
use gateway_cache_experiment_records::model::{Reason, Sample};
use gateway_core::identity::PoolVersionstamp;
use gateway_source::error::GatewayError;
use serde_json::json;
use std::future::Future;
pub struct ReadSample {
    pub value: Result<Sample, Reason>,
    pub completed: bool,
}
pub trait ExperimentIo {
    type Response;
    fn release_git_sha(&self) -> &str;
    fn uuid(&self) -> Result<String, GatewayError>;
    fn begin_cycle(&self, deadline_ms: i64) -> Result<(), GatewayError>;
    fn fetch(
        &self,
        dispatch: &CacheDispatch,
        body: &str,
        conversation: &str,
        client_version: Option<&str>,
        deadline_ms: i64,
    ) -> impl Future<Output = Result<Self::Response, GatewayError>>;
    fn observed_response(
        &self,
        dispatch: &CacheDispatch,
        response: &mut Self::Response,
    ) -> impl Future<Output = Result<(), GatewayError>>;
    fn discard(&self, response: Self::Response);
    fn read(
        &self,
        response: Self::Response,
        slot: u8,
        model: &str,
        index: usize,
        deadline_ms: i64,
    ) -> impl Future<Output = Result<ReadSample, GatewayError>>;
    fn refresh(
        &self,
        permit: RefreshPermit,
    ) -> impl Future<Output = Result<PoolVersionstamp, GatewayError>>;
}
pub fn request_body(model: &str, cycle: &str, key: &str) -> String {
    let prefix = std::iter::repeat_n("cache", 2_560)
        .collect::<Vec<_>>()
        .join(" ");
    gateway_schema::util::json_stringify(
        &json!({"model":model,"input":[{"type":"message","role":"developer","content":[{"type":"input_text","text":format!("cache-scope-cycle:{cycle}\n\n{prefix}")}]},{"type":"message","role":"user","content":[{"type":"input_text","text":"Reply with exactly: cache scope experiment."}]}],"store":false,"stream":true,"reasoning":{"effort":"none"},"prompt_cache_key":key}),
    )
}
