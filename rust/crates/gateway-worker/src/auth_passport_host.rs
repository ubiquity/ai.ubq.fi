//! Auth-only fixed transport. Preserve PlatformHost's actual cancellation and
//! store; bounds here apply only to new Passport app-session operations.
use crate::{
    host::{PlatformHost, response_bytes},
    transport::outbound::{PinnedDestination, send_http},
};
use gateway_auth_contracts::upstream::{AuthUpstreamRequest, AuthUpstreamResponse};
use gateway_auth_passport_contracts::session::PassportDate;
use gateway_source::{error::GatewayError, js_string::JsString16};
use gateway_worker_platform::cancellation::Cancellation;
use std::{cell::Cell, collections::BTreeMap, task::Poll};
use worker::{
    Method, ResponseBody,
    js_sys::{Function, JsString, Reflect},
    wasm_bindgen::{JsCast, JsValue},
};

impl PassportDate for PlatformHost {
    fn parse_date_ms(&self, value: &JsString16) -> f64 {
        // Date::parse(&str) narrows the source domain before the actual JS
        // primitive. Pass a JS UTF16 string directly to Date.parse instead.
        let parsed = (|| {
            let date = Reflect::get(&worker::js_sys::global(), &JsValue::from_str("Date")).ok()?;
            let parse = Reflect::get(&date, &JsValue::from_str("parse"))
                .ok()?
                .dyn_into::<Function>()
                .ok()?;
            parse
                .call1(&date, &JsString::from_char_code(value.as_units()))
                .ok()?
                .as_f64()
        })();
        parsed.unwrap_or(f64::NAN)
    }
}

pub async fn fetch(
    host: &PlatformHost,
    request: AuthUpstreamRequest,
) -> Result<AuthUpstreamResponse, GatewayError> {
    if !request.app_session_operation() {
        return fetch_inner(
            request,
            &host.cancellation,
            4 * 1024 * 1024,
            &Cell::new(false),
        )
        .await;
    }
    // Local logout has already committed. Its best-effort upstream revoke is
    // independent of caller cancellation; other operations retain the caller.
    let cancellation = if matches!(request, AuthUpstreamRequest::PassportRevoke { .. }) {
        Cancellation::background()?
    } else {
        host.cancellation.child()?
    };
    let _deadline = cancellation.set_timeout_ms(10_000)?;
    let rejected = Cell::new(false);
    let operation = Box::pin(fetch_inner(request, &cancellation, 65_536, &rejected));
    let interrupted = Box::pin(std::future::poll_fn(|cx| {
        cancellation.register_waker(cx.waker());
        if cancellation.signal().aborted() {
            Poll::Ready(())
        } else {
            Poll::Pending
        }
    }));
    let result = match futures_util::future::select(operation, interrupted).await {
        futures_util::future::Either::Left((result, _)) => result,
        // A rejected body cannot hide an already observed authoritative 401,
        // including when cancellation itself stalls until the deadline.
        futures_util::future::Either::Right(_) if rejected.get() => Ok(AuthUpstreamResponse {
            status: 401,
            body: Vec::new(),
        }),
        futures_util::future::Either::Right(_) => Err(unavailable()),
    };
    if result.is_err() {
        cancellation.abort();
    }
    result
}
async fn fetch_inner(
    request: AuthUpstreamRequest,
    cancellation: &Cancellation,
    limit: usize,
    rejected: &Cell<bool>,
) -> Result<AuthUpstreamResponse, GatewayError> {
    let headers = request.headers().into_iter().collect::<BTreeMap<_, _>>();
    let destination = PinnedDestination::new(&request.url())?;
    let method = if request.method() == "POST" {
        Method::Post
    } else {
        Method::Get
    };
    let mut response = send_http(
        &destination,
        method,
        &headers,
        &request.body()?,
        cancellation,
        || async { Ok(()) },
    )
    .await
    .map_err(|failure| failure.error)?;
    let status = response.status_code();
    if matches!(request, AuthUpstreamRequest::PassportPkceExchange { .. })
        && matches!(response.body(), ResponseBody::Empty)
    {
        return Err(GatewayError::new(
            502,
            "Passport verification is unavailable.",
            Some("passport_exchange_body_missing"),
        ));
    }
    if request.status_only(status) {
        rejected
            .set(matches!(request, AuthUpstreamRequest::PassportSession { .. }) && status == 401);
        let raw: worker::web_sys::Response = response.into();
        let cancelled = if let Some(body) = raw.body() {
            worker::wasm_bindgen_futures::JsFuture::from(body.cancel())
                .await
                .map(|_| ())
        } else {
            Ok(())
        };
        if matches!(request, AuthUpstreamRequest::PassportRevoke { .. }) {
            cancelled.map_err(|_| unavailable())?;
        }
        return Ok(AuthUpstreamResponse {
            status,
            body: Vec::new(),
        });
    }
    Ok(AuthUpstreamResponse {
        status,
        body: response_bytes(&mut response, limit).await?,
    })
}
fn unavailable() -> GatewayError {
    GatewayError::new(
        503,
        "Passport session verification is unavailable.",
        Some("passport_unavailable"),
    )
}
