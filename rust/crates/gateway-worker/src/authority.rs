//! Private Durable Object composition. Public requests never forward this protocol.
use crate::transport::body::request_bytes;
use gateway_core::{
    error::GatewayError,
    query::{QueryRequest, QueryStore},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, RevisionCheck,
    },
};
use gateway_worker_authority::wire::*;
use gateway_worker_storage::sqlite::SqliteAuthorityStore;
use worker::{DurableObject, Env, Method, Request, Response, State, durable_object};
#[durable_object]
pub struct GatewayAuthority {
    state: State,
    ingress: bool,
    waiters: std::rc::Rc<std::cell::RefCell<Vec<std::rc::Weak<DrainWaiter>>>>,
}

impl DurableObject for GatewayAuthority {
    fn new(state: State, env: Env) -> Self {
        Self {
            state,
            ingress: !crate::host::auth_config(&env).auth_tokens.is_empty(),
            waiters: Default::default(),
        }
    }

    async fn fetch(&self, mut request: Request) -> worker::Result<Response> {
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
        let bytes = match request_bytes(&mut request, WIRE_MAX_BYTES).await {
            Ok(bytes) => bytes,
            Err(error) => return error_response(&error),
        };
        if version.as_deref() == Some("2") {
            if let Ok(Envelope {
                access: _,
                request:
                    StoreRequest::Coordination {
                        request:
                            gateway_authority_provisioning::model::Operation::AwaitFrozen {
                                reference_id,
                            },
                    },
            }) = serde_json::from_slice(&bytes)
            {
                return self.await_response(reference_id).await;
            }
        }
        let result = async {
            let (access, message) = if version.as_deref() == Some("1") {
                (
                    gateway_authority_provisioning::model::Access::Legacy,
                    serde_json::from_slice(&bytes)
                        .map_err(|_| unavailable("authority_wire_invalid"))?,
                )
            } else {
                let envelope: Envelope = serde_json::from_slice(&bytes)
                    .map_err(|_| unavailable("authority_wire_invalid"))?;
                (envelope.access, envelope.request)
            };
            let identity = gateway_authority_provisioning::model::Identity {
                object_id: self.state.id().to_string(),
                source_sha: env!("UOS_RUST_BUILD_GIT_SHA").into(),
                source_tree: env!("UOS_RUST_BUILD_GIT_TREE").into(),
            };
            let store =
                SqliteAuthorityStore::open(self.state.storage(), identity, access, self.ingress)
                    .await?;
            match message {
                StoreRequest::Coordination { request } => {
                    let reply = gateway_worker_storage::drain::execute(
                        self.state.storage(),
                        request,
                        self.ingress,
                    )
                    .await?;
                    self.notify(&reply);
                    Ok(StoreResponse::Coordination { reply })
                }
                StoreRequest::ProfileMigration {
                    request,
                    request_id,
                    now_ms,
                } => {
                    let result = crate::migration_host::execute_profile(
                        &store,
                        request.into(),
                        gateway_admin_runtime::capabilities::Context { request_id, now_ms },
                    )
                    .await?;
                    if result.stream.is_some() {
                        return Err(unavailable("authority_wire_invalid"));
                    }
                    Ok(StoreResponse::ProfileMigration {
                        status: result.status,
                        headers: result.headers,
                        body: gateway_core::source_json::SourceJson::parse_bytes(
                            &result.body.to_bytes()?,
                        )?,
                    })
                }
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
}

fn error_response(error: &GatewayError) -> worker::Result<Response> {
    Ok(Response::from_json(&error.envelope)?.with_status(error.status))
}
fn unavailable(code: &str) -> GatewayError {
    GatewayError::unavailable("The shared coordination authority is unavailable.", code)
}

struct DrainWaiter {
    id: String,
    reply: std::cell::RefCell<Option<gateway_authority_provisioning::model::Reply>>,
    waker: futures_util::task::AtomicWaker,
}
impl GatewayAuthority {
    fn notify(&self, reply: &gateway_authority_provisioning::model::Reply) {
        use gateway_authority_provisioning::model::{Phase, Reply};
        let reference = match reply {
            Reply::Frozen { reference } => Some(reference),
            Reply::Status { status } | Reply::Finished { status, .. }
                if status.gate.phase == Phase::Frozen =>
            {
                status.gate.frozen.as_ref()
            }
            _ => None,
        };
        let Some(reference) = reference else {
            return;
        };
        self.waiters.borrow_mut().retain(|weak| {
            let Some(waiter) = weak.upgrade() else {
                return false;
            };
            if waiter.id != reference.freeze_id {
                return true;
            }
            *waiter.reply.borrow_mut() = Some(Reply::Frozen {
                reference: reference.clone(),
            });
            waiter.waker.wake();
            false
        });
    }
    async fn await_response(&self, id: String) -> worker::Result<Response> {
        use gateway_authority_provisioning::model::{Operation, Phase, Reply};
        let waiter = std::rc::Rc::new(DrainWaiter {
            id: id.clone(),
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
        let status = gateway_worker_storage::drain::execute(
            self.state.storage(),
            Operation::Status,
            self.ingress,
        )
        .await
        .map_err(|error| worker::Error::RustError(error.envelope.error.message))?;
        let Reply::Status { status } = status else {
            return Response::error("Drain status is invalid", 503);
        };
        if status.gate.phase == Phase::Frozen
            && status
                .gate
                .frozen
                .as_ref()
                .is_some_and(|value| value.freeze_id == id)
        {
            *waiter.reply.borrow_mut() = Some(Reply::Frozen {
                reference: status
                    .gate
                    .frozen
                    .clone()
                    .ok_or_else(|| worker::Error::RustError("Frozen receipt absent".into()))?,
            });
        } else if status.gate.phase != Phase::Draining
            || status.gate.drain_digest.as_deref() != Some(id.as_str())
        {
            return Response::error("Drain identity is stale", 409);
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
                    let latest =
                        gateway_worker_storage::drain::execute(storage, Operation::Status, ingress)
                            .await
                            .map_err(|error| {
                                worker::Error::RustError(error.envelope.error.message)
                            })?;
                    let Reply::Status { status } = latest else {
                        return Err(worker::Error::RustError("Drain status unavailable".into()));
                    };
                    Reply::DrainTimeout { status }
                }
            };
            serde_json::to_vec(&StoreResponse::Coordination { reply })
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
