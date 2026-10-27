//! The existing request cancellation, deadline and finite wait implementations.
use crate::{clock::monotonic, error::configuration_error};
use futures_util::task::AtomicWaker;
use gateway_admission::{AdmissionCancellation, AdmissionWait, WaitFuture};
use gateway_core::error::GatewayError;
use gateway_worker_stream::control::StreamControl;
use std::{
    future::{Future, poll_fn},
    pin::pin,
    rc::Rc,
    task::{Poll, Waker},
    time::Duration,
};
use worker::{
    AbortSignal, Request,
    js_sys::Reflect,
    wasm_bindgen::{JsCast, JsValue, closure::Closure},
};

/// Owns event listeners until the whole upstream body/stream finishes, not only
/// until response headers arrive. Drop removes the listener without leaking it.
pub struct Cancellation {
    controller: worker::web_sys::AbortController,
    incoming: worker::web_sys::AbortSignal,
    listener: Closure<dyn FnMut()>,
    waker: Rc<AtomicWaker>,
}

impl Cancellation {
    pub fn from_request(request: &Request) -> Result<Self, GatewayError> {
        Self::from_signal(request.inner().signal())
    }

    pub fn child(&self) -> Result<Self, GatewayError> {
        Self::from_signal(self.controller.signal())
    }

    /// Event-owned billing is independent of a caller's delivery cancellation.
    pub fn background() -> Result<Self, GatewayError> {
        let incoming =
            worker::web_sys::AbortController::new().map_err(|_| configuration_error())?;
        Self::from_signal(incoming.signal())
    }

    fn from_signal(incoming: worker::web_sys::AbortSignal) -> Result<Self, GatewayError> {
        let controller =
            worker::web_sys::AbortController::new().map_err(|_| configuration_error())?;
        let forwarded = controller.clone();
        let waker = Rc::new(AtomicWaker::new());
        let wake = waker.clone();
        let listener = Closure::wrap(Box::new(move || {
            forwarded.abort();
            wake.wake();
        }) as Box<dyn FnMut()>);
        incoming
            .add_event_listener_with_callback("abort", listener.as_ref().unchecked_ref())
            .map_err(|_| configuration_error())?;
        if incoming.aborted() {
            controller.abort();
        }
        Ok(Self {
            controller,
            incoming,
            listener,
            waker,
        })
    }

    pub fn signal(&self) -> AbortSignal {
        self.controller.signal().into()
    }

    pub fn abort(&self) {
        self.controller.abort();
        self.waker.wake();
    }

    /// A quiet upstream may never produce another chunk after caller abort.
    /// Wake the outgoing Rust stream directly from the actual platform signal.
    pub fn register_waker(&self, waker: &Waker) {
        self.waker.register(waker);
    }

    pub fn client_aborted(&self) -> bool {
        self.incoming.aborted()
    }

    /// Remaining time computed from the route's absolute host-policy deadline.
    pub fn set_timeout_ms(&self, milliseconds: i32) -> Result<Deadline, GatewayError> {
        if milliseconds < 0 {
            return Err(configuration_error());
        }
        let controller = self.controller.clone();
        let wake = self.waker.clone();
        let callback = Closure::wrap(Box::new(move || {
            controller.abort();
            wake.wake();
        }) as Box<dyn FnMut()>);
        let global = worker::js_sys::global();
        let set_timeout = Reflect::get(&global, &JsValue::from_str("setTimeout"))
            .map_err(|_| configuration_error())?
            .dyn_into::<worker::js_sys::Function>()
            .map_err(|_| configuration_error())?;
        let handle = set_timeout
            .call2(&global, callback.as_ref(), &JsValue::from(milliseconds))
            .map_err(|_| configuration_error())?;
        Ok(Deadline { handle, callback })
    }
}

impl Drop for Cancellation {
    fn drop(&mut self) {
        let _ = self
            .incoming
            .remove_event_listener_with_callback("abort", self.listener.as_ref().unchecked_ref());
    }
}

pub struct Deadline {
    handle: JsValue,
    callback: Closure<dyn FnMut()>,
}

impl Drop for Deadline {
    fn drop(&mut self) {
        let global = worker::js_sys::global();
        if let Ok(clear) = Reflect::get(&global, &JsValue::from_str("clearTimeout"))
            && let Some(clear) = clear.dyn_ref::<worker::js_sys::Function>()
        {
            let _ = clear.call1(&global, &self.handle);
        }
        // Keep the closure alive until its timer has been cleared.
        let _ = &self.callback;
    }
}

impl StreamControl for Cancellation {
    type Deadline = Deadline;
    fn signal(&self) -> worker::AbortSignal {
        Cancellation::signal(self)
    }
    fn register_waker(&self, waker: &std::task::Waker) {
        Cancellation::register_waker(self, waker);
    }
    fn client_aborted(&self) -> bool {
        Cancellation::client_aborted(self)
    }
    fn set_timeout_ms(&self, milliseconds: i32) -> Result<Deadline, GatewayError> {
        Cancellation::set_timeout_ms(self, milliseconds)
    }
    fn abort(&self) {
        Cancellation::abort(self);
    }
}

impl AdmissionCancellation for Cancellation {
    fn is_cancelled(&self) -> bool {
        self.client_aborted()
    }
}

impl AdmissionWait for Cancellation {
    fn wait_until<'a>(
        &'a self,
        deadline: Duration,
        cancellation: &'a dyn AdmissionCancellation,
    ) -> WaitFuture<'a> {
        Box::pin(async move {
            let mut delay = pin!(worker::Delay::from(
                deadline.saturating_sub(monotonic().unwrap_or(Duration::MAX))
            ));
            poll_fn(|context| {
                self.register_waker(context.waker());
                if cancellation.is_cancelled() {
                    Poll::Ready(())
                } else {
                    delay.as_mut().poll(context)
                }
            })
            .await;
        })
    }
}
