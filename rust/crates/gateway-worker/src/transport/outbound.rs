use gateway_core::error::GatewayError;
use gateway_worker_platform::{cancellation::Cancellation, error::configuration_error};
use std::collections::BTreeMap;
use worker::{
    Fetch, Headers, Method, Request, RequestInit, RequestRedirect, Response, Url,
    js_sys::Uint8Array,
};

/// Constructed by trusted provider/native-host configuration, not request JSON.
pub struct PinnedDestination {
    url: Url,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DispatchAttempt {
    NotStarted,
    /// The actual fetch API was invoked; transport failure does not prove no spend.
    Started,
}

pub struct DispatchFailure {
    pub error: GatewayError,
    pub attempt: DispatchAttempt,
    pub origin: DispatchOrigin,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DispatchOrigin {
    Gateway,
    Transport,
}

impl From<GatewayError> for DispatchFailure {
    fn from(error: GatewayError) -> Self {
        Self {
            error,
            attempt: DispatchAttempt::NotStarted,
            origin: DispatchOrigin::Gateway,
        }
    }
}

impl PinnedDestination {
    pub fn new(url: &str) -> Result<Self, GatewayError> {
        let url = Url::parse(url).map_err(|_| configuration_error())?;
        if !matches!(url.scheme(), "https" | "http")
            || url.host_str().is_none()
            || !url.username().is_empty()
            || url.password().is_some()
            || url.fragment().is_some()
        {
            return Err(configuration_error());
        }
        Ok(Self { url })
    }
}

pub async fn send_http<F, Fut>(
    destination: &PinnedDestination,
    method: Method,
    headers: &BTreeMap<String, String>,
    body: &[u8],
    cancellation: &Cancellation,
    before_dispatch: F,
) -> Result<Response, DispatchFailure>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Result<(), GatewayError>>,
{
    if cancellation.signal().aborted() {
        return Err(interrupted().into());
    }
    let wire_headers = Headers::new();
    for (name, value) in headers {
        wire_headers
            .set(name, value)
            .map_err(|_| configuration_error())?;
    }
    let request = construct_request(destination, method, wire_headers, body)
        .map_err(|_| configuration_error())?;
    send_constructed(request, cancellation, before_dispatch).await
}

/// Actual native Request already constructed at the original fixed physical target.
/// Private fields prevent a caller from manufacturing or serializing this value.
pub(crate) struct ConstructedRequest(Request);

pub(crate) fn construct_request(
    destination: &PinnedDestination,
    method: Method,
    headers: Headers,
    body: &[u8],
) -> worker::Result<ConstructedRequest> {
    let mut init = RequestInit::new();
    init.with_method(method)
        .with_headers(headers)
        .with_redirect(RequestRedirect::Manual);
    if !body.is_empty() {
        init.with_body(Some(Uint8Array::from(body).into()));
    }
    Request::new_with_init(destination.url.as_str(), &init).map(ConstructedRequest)
}

pub(crate) async fn send_constructed<F, Fut>(
    request: ConstructedRequest,
    cancellation: &Cancellation,
    before_dispatch: F,
) -> Result<Response, DispatchFailure>
where
    F: FnOnce() -> Fut,
    Fut: std::future::Future<Output = Result<(), GatewayError>>,
{
    let ConstructedRequest(request) = request;
    // Validate/build the request before recording dispatch. No await, retry,
    // fallback or destination change occurs between this hook and actual fetch.
    before_dispatch().await?;
    if cancellation.signal().aborted() {
        return Err(interrupted().into());
    }
    Fetch::Request(request)
        .send_with_signal(&cancellation.signal())
        .await
        .map_err(|_| DispatchFailure {
            error: if cancellation.signal().aborted() {
                interrupted()
            } else {
                gateway_worker_platform::error::platform_error(
                    "The upstream transport failed.",
                    "upstream_transport_error",
                )
            },
            attempt: DispatchAttempt::Started,
            origin: DispatchOrigin::Transport,
        })
}

fn interrupted() -> GatewayError {
    gateway_worker_platform::error::platform_error(
        "The upstream request was interrupted.",
        "upstream_request_interrupted",
    )
}
