//! Request-scoped access to the one durable authority through its private binding.
use crate::wire::*;
use futures_util::StreamExt;
use gateway_core::{
    error::GatewayError,
    query::{QueryEntry, QueryPage, QueryRequest, QueryStore},
    state::{
        AtomicTransition, AuthorityStore, CommitOutcome, RecordKey, RecordMutation, StoredRecord,
    },
};
use std::rc::Rc;
use worker::{Env, Headers, Method, Request, RequestInit, Stub, js_sys::Uint8Array};
#[derive(Clone)]
pub struct RemoteAuthorityStore {
    stub: Rc<Stub>,
    access: gateway_authority_lifecycle::model::Access,
    writers: Option<Rc<crate::writers::WriterGroup>>,
}

impl RemoteAuthorityStore {
    pub fn from_env(env: &Env) -> Result<Self, GatewayError> {
        let namespace = env
            .durable_object("AUTHORITY")
            .map_err(|_| unavailable("authority_binding_unavailable"))?;
        let stub = namespace
            .id_from_name(AUTHORITY_OBJECT)
            .and_then(|id| id.get_stub())
            .map_err(|_| unavailable("authority_binding_unavailable"))?;
        Ok(Self {
            stub: Rc::new(stub),
            access: gateway_authority_lifecycle::model::Access::Legacy,
            writers: None,
        })
    }

    pub async fn profile_migration(
        &self,
        request: gateway_admin_runtime::capabilities::MigrationRequest,
        context: gateway_admin_runtime::capabilities::Context,
    ) -> Result<gateway_auth::types::AuthResponse, GatewayError> {
        match self
            .exchange(StoreRequest::ProfileMigration {
                request: request.into(),
                request_id: context.request_id,
                now_ms: context.now_ms,
            })
            .await?
        {
            StoreResponse::ProfileMigration {
                status,
                headers,
                body,
            } => {
                let mut response = gateway_auth::types::AuthResponse::source_json(status, body);
                response.headers = headers;
                Ok(response)
            }
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }

    pub fn scoped(&self, access: gateway_authority_lifecycle::model::Access) -> Self {
        Self {
            stub: self.stub.clone(),
            access,
            writers: self.writers.clone(),
        }
    }
    pub fn access(&self) -> &gateway_authority_lifecycle::model::Access {
        &self.access
    }
    pub fn operator_read(&self) -> Self {
        self.scoped(gateway_authority_lifecycle::model::Access::OperatorRead)
    }
    pub async fn coordination(
        &self,
        request: gateway_authority_lifecycle::model::Operation,
    ) -> Result<gateway_authority_lifecycle::model::Reply, GatewayError> {
        match self
            .exchange(StoreRequest::Coordination { request })
            .await?
        {
            StoreResponse::Coordination { reply } => Ok(reply),
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }
    pub async fn acquire(
        &self,
        request: gateway_authority_lifecycle::model::Acquire,
        capability: String,
    ) -> Result<Self, GatewayError> {
        let id = request.id.clone();
        match self
            .coordination(gateway_authority_lifecycle::model::Operation::Acquire { request })
            .await?
        {
            gateway_authority_lifecycle::model::Reply::Acquired { serving_epoch } => Ok(self
                .scoped(gateway_authority_lifecycle::model::Access::Permit(
                    gateway_authority_lifecycle::model::Permit {
                        id,
                        serving_epoch,
                        capability,
                    },
                ))),
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }
    pub async fn join_native(
        &self,
        selector: gateway_authority_lifecycle::model::NativeSelector,
        id: String,
        capability: String,
    ) -> Result<Self, GatewayError> {
        let digest = gateway_auth::crypto::sha256_hex(capability.as_bytes());
        match self
            .coordination(gateway_authority_lifecycle::model::Operation::JoinNative {
                selector,
                id: id.clone(),
                capability_sha256: digest,
            })
            .await?
        {
            gateway_authority_lifecycle::model::Reply::Acquired { serving_epoch } => Ok(self
                .scoped(gateway_authority_lifecycle::model::Access::Permit(
                    gateway_authority_lifecycle::model::Permit {
                        id,
                        serving_epoch,
                        capability,
                    },
                ))),
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }
    pub fn permit(&self) -> Result<gateway_authority_lifecycle::model::Permit, GatewayError> {
        match &self.access {
            gateway_authority_lifecycle::model::Access::Permit(value) => Ok(value.clone()),
            _ => Err(unavailable("authority_permit_unavailable")),
        }
    }
    pub fn track_root(
        mut self,
        context: &worker::Context,
    ) -> Result<(Self, crate::writers::Writer), GatewayError> {
        let group = crate::writers::WriterGroup::new();
        let root = group.register()?;
        group.retain(self.clone(), self.permit()?, context);
        self.writers = Some(group);
        Ok((self, root))
    }
    pub fn writer(&self) -> Result<Option<crate::writers::Writer>, GatewayError> {
        self.writers
            .as_ref()
            .map(crate::writers::WriterGroup::register)
            .transpose()
    }
    pub fn required_failure(&self) {
        if let Some(group) = &self.writers {
            group.block(gateway_authority_lifecycle::model::Blocker::RequiredSettlement);
        }
    }
    pub fn spawn_writer<F: std::future::Future<Output = ()> + 'static>(
        &self,
        context: &worker::Context,
        work: F,
    ) -> Result<(), GatewayError> {
        let writer = self.writer()?;
        context.wait_until(async move {
            work.await;
            if let Some(writer) = writer {
                writer.complete();
            }
        });
        Ok(())
    }
    pub async fn finish_scope(
        &self,
        blocker: Option<gateway_authority_lifecycle::model::Blocker>,
    ) -> Result<(), GatewayError> {
        self.coordination(gateway_authority_lifecycle::model::Operation::Finish {
            permit: self.permit()?,
            blocker,
        })
        .await
        .map(drop)
    }
    async fn exchange(&self, message: StoreRequest) -> Result<StoreResponse, GatewayError> {
        let bytes = serde_json::to_vec(&Envelope {
            access: self.access.clone(),
            request: message,
        })
        .map_err(|_| unavailable("authority_wire_invalid"))?;
        if bytes.len() > WIRE_MAX_BYTES {
            return Err(unavailable("authority_wire_too_large"));
        }
        let headers = Headers::new();
        headers
            .set("Content-Type", "application/json")
            .map_err(|_| unavailable("authority_wire_invalid"))?;
        headers
            .set(WIRE_VERSION_HEADER, WIRE_VERSION)
            .map_err(|_| unavailable("authority_wire_invalid"))?;
        let mut init = RequestInit::new();
        init.with_method(Method::Post)
            .with_headers(headers)
            .with_body(Some(Uint8Array::from(bytes.as_slice()).into()));
        let request = Request::new_with_init(WIRE_URL, &init)
            .map_err(|_| unavailable("authority_wire_invalid"))?;
        let mut response = self
            .stub
            .fetch_with_request(request)
            .await
            .map_err(|_| unavailable("authority_storage_unavailable"))?;
        if response.status_code() != 200
            || response
                .headers()
                .get(WIRE_VERSION_HEADER)
                .map_err(|_| unavailable("authority_wire_invalid"))?
                .as_deref()
                != Some(WIRE_VERSION)
        {
            return Err(unavailable("authority_storage_unavailable"));
        }
        let mut stream = response
            .stream()
            .map_err(|_| unavailable("authority_wire_invalid"))?;
        let mut bytes = Vec::new();
        while let Some(chunk) = stream.next().await {
            let chunk = chunk.map_err(|_| unavailable("authority_storage_unavailable"))?;
            if bytes
                .len()
                .checked_add(chunk.len())
                .is_none_or(|size| size > WIRE_MAX_BYTES)
            {
                return Err(unavailable("authority_wire_too_large"));
            }
            bytes.extend_from_slice(&chunk);
        }
        serde_json::from_slice(&bytes).map_err(|_| unavailable("authority_wire_invalid"))
    }
}

impl AuthorityStore for RemoteAuthorityStore {
    async fn read(&self, key: &RecordKey) -> Result<Option<StoredRecord>, GatewayError> {
        match self
            .exchange(StoreRequest::Read { key: key.0.clone() })
            .await?
        {
            StoreResponse::Read { record } => {
                if let Some(record) = &record {
                    record.validate_version()?;
                }
                Ok(record)
            }
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }

    async fn commit(&self, transition: AtomicTransition) -> Result<CommitOutcome, GatewayError> {
        let checks = transition
            .checks
            .into_iter()
            .map(|check| WireCheck {
                key: check.key.0,
                expected_revision: check.expected_revision,
            })
            .collect();
        let mutations = transition
            .mutations
            .into_iter()
            .map(|mutation| match mutation {
                RecordMutation::Put { key, record } => WireMutation::Put { key: key.0, record },
                RecordMutation::Delete { key } => WireMutation::Delete { key: key.0 },
            })
            .collect();
        let result = self
            .exchange(StoreRequest::Commit { checks, mutations })
            .await;
        if result.is_err() {
            if let Some(group) = &self.writers {
                group.block(gateway_authority_lifecycle::model::Blocker::UncertainCommit);
            }
        }
        match result? {
            StoreResponse::Commit { committed } => Ok(if committed {
                CommitOutcome::Committed
            } else {
                CommitOutcome::Conflict
            }),
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }
}

impl QueryStore for RemoteAuthorityStore {
    async fn query(&self, request: QueryRequest) -> Result<QueryPage, GatewayError> {
        let validated = request.validate()?;
        let QueryRequest {
            selector,
            reverse,
            limit,
            cursor,
            now_ms,
        } = request;
        match self
            .exchange(StoreRequest::Query {
                selector: selector.into(),
                reverse,
                limit,
                cursor,
                now_ms,
            })
            .await?
        {
            StoreResponse::Query {
                entries,
                next_cursor,
            } => {
                if entries.len() > limit as usize {
                    return Err(unavailable("authority_wire_invalid"));
                }
                let entries = entries
                    .into_iter()
                    .map(|entry| QueryEntry {
                        key: RecordKey(entry.key),
                        record: entry.record,
                    })
                    .collect::<Vec<_>>();
                let mut previous = None;
                for entry in &entries {
                    if !validated.accepts_record(&entry.key, &entry.record)? {
                        return Err(unavailable("authority_wire_invalid"));
                    }
                    let tuple = gateway_core::tuple_key::TupleKey::from_record_key(&entry.key)?;
                    if previous.as_ref().is_some_and(|old| {
                        if reverse {
                            tuple >= *old
                        } else {
                            tuple <= *old
                        }
                    }) {
                        return Err(unavailable("authority_wire_invalid"));
                    }
                    previous = Some(tuple);
                }
                if let Some(cursor) = &next_cursor {
                    let last = entries
                        .last()
                        .ok_or_else(|| unavailable("authority_wire_invalid"))?;
                    if validated.cursor_after(&last.key)? != *cursor {
                        return Err(unavailable("authority_wire_invalid"));
                    }
                }
                Ok(QueryPage {
                    entries,
                    next_cursor,
                })
            }
            _ => Err(unavailable("authority_wire_invalid")),
        }
    }
}

fn unavailable(code: &str) -> GatewayError {
    GatewayError::unavailable("The shared coordination authority is unavailable.", code)
}
