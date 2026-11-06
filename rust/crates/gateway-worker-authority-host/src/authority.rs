//! Private Durable Object composition. Public requests never forward this protocol.
use gateway_authority_lifecycle::model::{DrainPlan, Identity};
use gateway_core::{
    error::GatewayError,
    query::{QueryRequest, QueryStore},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
    },
};
use gateway_worker_authority::wire::{
    Envelope, ProfileMigration, StoreRequest, StoreResponse, WIRE_MAX_BYTES, WIRE_VERSION,
    WIRE_VERSION_HEADER, WireEntry, WireMutation,
};
use gateway_worker_storage::sqlite::SqliteAuthorityStore;
use worker::{Method, Request, Response, State};

pub trait Host {
    fn request_bytes(
        request: &mut Request,
        max: usize,
    ) -> impl std::future::Future<Output = Result<Vec<u8>, GatewayError>>;
    fn profile_migration(
        store: &SqliteAuthorityStore,
        request: ProfileMigration,
        request_id: String,
        now_ms: i64,
    ) -> impl std::future::Future<Output = Result<StoreResponse, GatewayError>>;
}
pub struct Authority<H> {
    identity: Identity,
    _host: std::marker::PhantomData<fn() -> H>,
    state: State,
    ingress: bool,
    waiters: std::rc::Rc<std::cell::RefCell<Vec<std::rc::Weak<DrainWaiter>>>>,
}

impl<H: Host> Authority<H> {
    pub fn new(state: State, identity: Identity, ingress: bool) -> Self {
        Self {
            state,
            identity,
            ingress,
            _host: std::marker::PhantomData,
            waiters: Default::default(),
        }
    }

    pub async fn fetch(&self, mut request: Request) -> worker::Result<Response> {
        if request.path() != "/v1/store" {
            return Response::error("Not found", 404);
        }
        if request.method() != Method::Post {
            return Response::error("Method not allowed", 405);
        }
        let version = request.headers().get(WIRE_VERSION_HEADER)?;
        if !matches!(version.as_deref(), Some("1" | "2")) {
            return error_response(&GatewayError::unavailable(
                "The authority protocol version is unsupported.",
                "authority_wire_version_unsupported",
            ));
        }
        let bytes = match H::request_bytes(&mut request, WIRE_MAX_BYTES).await {
            Ok(bytes) => bytes,
            Err(error) => return error_response(&error),
        };
        if version.as_deref() == Some("2")
            && let Ok(Envelope {
                access: _,
                request:
                    StoreRequest::Coordination {
                        request:
                            gateway_authority_lifecycle::model::Operation::AwaitFrozen {
                                reference_id,
                                plan,
                            },
                    },
            }) = serde_json::from_slice(&bytes)
        {
            return self.await_response(reference_id, plan).await;
        }
        let result = async {
            let (access, message) = if version.as_deref() == Some("1") {
                (
                    gateway_authority_lifecycle::model::Access::Legacy,
                    serde_json::from_slice(&bytes)
                        .map_err(|_| unavailable("authority_wire_invalid"))?,
                )
            } else {
                let envelope: Envelope = serde_json::from_slice(&bytes)
                    .map_err(|_| unavailable("authority_wire_invalid"))?;
                (envelope.access, envelope.request)
            };
            let identity = self.identity.clone();
            let store =
                SqliteAuthorityStore::open(self.state.storage(), identity, access, self.ingress)
                    .await?;
            self.dispatch(&store, message).await
        }
        .await;
        match result {
            Ok(message) => {
                let mut response = Response::from_json(&message)?;
                response.headers_mut().set("Cache-Control", "no-store")?;
                response
                    .headers_mut()
                    .set(WIRE_VERSION_HEADER, WIRE_VERSION)?;
                Ok(response)
            }
            Err(error) => error_response(&error),
        }
    }
    async fn dispatch(
        &self,
        store: &SqliteAuthorityStore,
        message: StoreRequest,
    ) -> Result<StoreResponse, GatewayError> {
        match message {
            StoreRequest::Coordination { request } => {
                let reply = gateway_worker_drain::control::execute(
                    self.state.storage(),
                    request,
                    self.ingress,
                    gateway_worker_storage::sqlite::read_control_record,
                )
                .await?;
                self.notify(&reply);
                Ok(StoreResponse::Coordination {
                    reply: Box::new(reply),
                })
            }
            StoreRequest::ProfileMigration {
                request,
                request_id,
                now_ms,
            } => H::profile_migration(store, request, request_id, now_ms).await,
            StoreRequest::Read { key } => Ok(StoreResponse::Read {
                record: store.read(&RecordKey(key)).await?,
            }),
            StoreRequest::Commit { checks, mutations } => {
                let transition = AtomicTransition {
                    checks: checks
                        .into_iter()
                        .map(|check| RevisionCheck {
                            key: RecordKey(check.key),
                            expected_revision: check.expected_revision,
                        })
                        .collect(),
                    mutations: mutations
                        .into_iter()
                        .map(|mutation| match mutation {
                            WireMutation::Put { key, record } => RecordMutation::Put {
                                key: RecordKey(key),
                                record,
                            },
                            WireMutation::Delete { key } => RecordMutation::Delete {
                                key: RecordKey(key),
                            },
                        })
                        .collect(),
                };
                Ok(StoreResponse::Commit {
                    committed: store.commit(transition).await? == CommitOutcome::Committed,
                })
            }
            StoreRequest::Query {
                selector,
                reverse,
                limit,
                cursor,
                now_ms,
            } => {
                let page = store
                    .query(QueryRequest {
                        selector: selector.into(),
                        reverse,
                        limit,
                        cursor,
                        now_ms,
                    })
                    .await?;
                Ok(StoreResponse::Query {
                    entries: page
                        .entries
                        .into_iter()
                        .map(|entry| WireEntry {
                            key: entry.key.0,
                            record: entry.record,
                        })
                        .collect(),
                    next_cursor: page.next_cursor,
                })
            }
        }
    }
}

fn error_response(error: &GatewayError) -> worker::Result<Response> {
    Ok(Response::from_json(&error.envelope)?.with_status(error.status))
}
fn unavailable(code: &str) -> GatewayError {
    GatewayError::unavailable("The shared coordination authority is unavailable.", code)
}

struct DrainWaiter {
    id: String,
    plan: DrainPlan,
    reply: std::cell::RefCell<Option<gateway_authority_lifecycle::model::Reply>>,
    waker: futures_util::task::AtomicWaker,
}
impl<H: Host> Authority<H> {
    fn notify(&self, reply: &gateway_authority_lifecycle::model::Reply) {
        use gateway_authority_lifecycle::model::{Phase, Reply};
        self.waiters.borrow_mut().retain(|weak| {
            let Some(waiter) = weak.upgrade() else {
                return false;
            };
            let terminal = match reply {
                Reply::Cancelled {
                    cancellation,
                    status,
                    ..
                } if cancellation.plan.binding == waiter.plan.binding
                    && cancellation.plan.drain_revision == waiter.plan.drain_revision
                    && cancellation.plan.drain_digest == waiter.id =>
                {
                    Some(Reply::DrainCancelled {
                        cancellation: cancellation.clone(),
                        status: status.clone(),
                    })
                }
                Reply::Frozen { reference }
                    if reference.freeze_id == waiter.id
                        && reference.identity == waiter.plan.binding.identity
                        && reference.serving_epoch == waiter.plan.binding.serving_epoch =>
                {
                    Some(Reply::Frozen {
                        reference: reference.clone(),
                    })
                }
                Reply::Status { status } | Reply::Finished { status, .. }
                    if status.gate.phase == Phase::Frozen
                        && status.gate.drain_revision == waiter.plan.drain_revision =>
                {
                    status
                        .gate
                        .frozen
                        .as_ref()
                        .filter(|reference| {
                            reference.freeze_id == waiter.id
                                && reference.identity == waiter.plan.binding.identity
                                && reference.serving_epoch == waiter.plan.binding.serving_epoch
                        })
                        .map(|reference| Reply::Frozen {
                            reference: reference.clone(),
                        })
                }
                _ => None,
            };
            if let Some(terminal) = terminal {
                *waiter.reply.borrow_mut() = Some(terminal);
                waiter.waker.wake();
                false
            } else {
                true
            }
        });
    }
    fn terminal(
        status: &gateway_authority_lifecycle::model::Status,
        plan: &DrainPlan,
        id: &str,
    ) -> Option<gateway_authority_lifecycle::model::Reply> {
        use gateway_authority_lifecycle::{
            maintenance,
            model::{Phase, Reply},
            policy,
        };
        if policy::binding(&status.gate, &plan.binding).is_err() {
            return Some(Reply::DrainStale {
                status: status.clone(),
            });
        }
        if let Some(cancellation) = maintenance::cancelled(&status.gate, plan, id) {
            return Some(Reply::DrainCancelled {
                cancellation,
                status: status.clone(),
            });
        }
        if status.gate.phase == Phase::Frozen
            && status.gate.drain_revision == plan.drain_revision
            && let Some(reference) = status
                .gate
                .frozen
                .as_ref()
                .filter(|value| value.freeze_id == id)
        {
            return Some(Reply::Frozen {
                reference: reference.clone(),
            });
        }
        if status.gate.phase != Phase::Draining
            || status.gate.drain_revision != plan.drain_revision
            || status.gate.drain_digest.as_deref() != Some(id)
        {
            return Some(Reply::DrainStale {
                status: status.clone(),
            });
        }
        None
    }
    async fn await_response(&self, id: String, plan: DrainPlan) -> worker::Result<Response> {
        use gateway_authority_lifecycle::model::{Operation, Reply};
        let waiter = std::rc::Rc::new(DrainWaiter {
            id: id.clone(),
            plan: plan.clone(),
            reply: Default::default(),
            waker: Default::default(),
        });
        {
            let mut waiters = self.waiters.borrow_mut();
            waiters.retain(|value| value.strong_count() != 0);
            if waiters.len() >= 64 {
                return Response::error("Drain waiter capacity is full", 503);
            }
            waiters.push(std::rc::Rc::downgrade(&waiter));
        }
        let status = gateway_worker_drain::control::execute(
            self.state.storage(),
            Operation::Status,
            self.ingress,
            gateway_worker_storage::sqlite::read_control_record,
        )
        .await
        .map_err(|error| worker::Error::RustError(error.envelope.error.message))?;
        let Reply::Status { status } = status else {
            return Response::error("Drain status is invalid", 503);
        };
        if let Some(reply) = Self::terminal(&status, &plan, &id) {
            *waiter.reply.borrow_mut() = Some(reply);
        }
        let storage = self.state.storage();
        let ingress = self.ingress;
        let stream = futures_util::stream::once(async move {
            let notified = std::future::poll_fn(|cx| {
                waiter.waker.register(cx.waker());
                waiter
                    .reply
                    .borrow_mut()
                    .take()
                    .map_or(std::task::Poll::Pending, std::task::Poll::Ready)
            });
            let deadline = worker::Delay::from(std::time::Duration::from_millis(300_000));
            let reply = match futures_util::future::select(Box::pin(notified), Box::pin(deadline))
                .await
            {
                futures_util::future::Either::Left((reply, _)) => reply,
                futures_util::future::Either::Right(_) => {
                    let latest = gateway_worker_drain::control::execute(
                        storage,
                        Operation::Status,
                        ingress,
                        gateway_worker_storage::sqlite::read_control_record,
                    )
                    .await
                    .map_err(|error| worker::Error::RustError(error.envelope.error.message))?;
                    let Reply::Status { status } = latest else {
                        return Err(worker::Error::RustError("Drain status unavailable".into()));
                    };
                    Self::terminal(&status, &plan, &id).unwrap_or(Reply::DrainTimeout { status })
                }
            };
            serde_json::to_vec(&StoreResponse::Coordination {
                reply: Box::new(reply),
            })
            .map_err(|error| worker::Error::RustError(error.to_string()))
        });
        let mut response = Response::from_stream(stream)?;
        response.headers_mut().set("Cache-Control", "no-store")?;
        response
            .headers_mut()
            .set(WIRE_VERSION_HEADER, WIRE_VERSION)?;
        Ok(response)
    }
}
