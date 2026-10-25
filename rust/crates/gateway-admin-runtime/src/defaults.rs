use crate::{
    AdminRuntime,
    capabilities::RuntimeCapabilities,
    kernel::{default_limit_key, default_window_key},
    parse, store,
};
use gateway_admin::{operations::AdminInput, routes::AdminOperation, source_input as source_body};
use gateway_auth::store::{check, commit, put};
use gateway_auth::types::{AuthHost, AuthResponse, conflict, invalid};
use gateway_authority::{kernel::KernelDefaults, keys};
use gateway_core::error::GatewayError;
use gateway_core::js_string::JsString16;
use gateway_core::model::ModelId;
use gateway_core::query::QueryStore;
use gateway_core::source_control::{decode_source_control_record, encode_source_control_record};
use gateway_core::source_json::SourceJson;
use gateway_core::state::{RecordKey, RecordMutation, StoredRecord};
use gateway_schema::catalog_store::runtime_config_key;
use gateway_schema::reasoning::ModelReasoning;
use gateway_schema::runtime_config::{
    build_runtime_config, default_model, default_reasoning_effort, normalize_runtime_config,
};
use gateway_schema::snapshot::ModelSnapshot;
use serde_json::json;

fn response(model: &ModelId, effort: &JsString16, defaults: &KernelDefaults) -> SourceJson {
    source_body::object(vec![(
        "defaults",
        source_body::object(vec![
            ("model", source_body::json(model)),
            ("reasoning_effort", source_body::json(effort)),
            (
                "kernel_policy_limit_requests",
                source_body::json(&defaults.usage_limit_requests),
            ),
            (
                "kernel_policy_window_ms",
                source_body::json(&defaults.window_ms),
            ),
        ]),
    )])
}
fn runtime_value(row: Option<&StoredRecord>, now: i64) -> Result<Option<SourceJson>, GatewayError> {
    let Some(row) = row else { return Ok(None) };
    row.validate_version()?;
    if row.expires_at_ms.is_some_and(|expiry| expiry <= now) {
        return Ok(None);
    }
    Ok(normalize_runtime_config(&decode_source_control_record(
        row,
    )?))
}
impl<S: QueryStore, H: AuthHost, C: RuntimeCapabilities> AdminRuntime<'_, S, H, C> {
    pub(crate) async fn defaults(
        &self,
        op: AdminOperation,
        input: &AdminInput,
    ) -> Result<AuthResponse, GatewayError> {
        if op == AdminOperation::DefaultsGet {
            let row = self.store.read(&runtime_config_key()).await?;
            let runtime = runtime_value(row.as_ref(), input.now_ms)?;
            let defaults = self.kernel_defaults(input.now_ms).await?;
            let (model, effort) = model_defaults(runtime.as_ref());
            let mut body = response(&model, &effort, &defaults);
            source_body::set(
                &mut body,
                "metered_quota",
                source_body::json(&self.capabilities.metered_diagnostics().await?),
            );
            return Ok(AuthResponse::source_json(200, body));
        }
        let body = &input.body;
        if let Some(error) = validate_defaults_fields(body)? {
            return Ok(error);
        }
        let limit=source_body::member(body,"kernel_policy_limit_requests").as_ref().map(|value|parse::source_limit(value).ok_or_else(||invalid("kernel_policy_limit_requests must be a non-negative number or -1 for unlimited"))).transpose()?;
        let window = source_body::member(body, "kernel_policy_window_ms")
            .as_ref()
            .map(|value| {
                parse::source_positive(value)
                    .ok_or_else(|| invalid("kernel_policy_window_ms must be a positive number"))
            })
            .transpose()?;
        for _ in 0..3 {
            let rk = runtime_config_key();
            let rr = self.store.read(&rk).await?;
            let lk = default_limit_key();
            let lr = self.store.read(&lk).await?;
            let wk = default_window_key();
            let wr = self.store.read(&wk).await?;
            let dk = keys::kernel_defaults();
            let dr = self.store.read(&dk).await?;
            let runtime = runtime_value(rr.as_ref(), input.now_ms)?;
            let old = self.kernel_defaults(input.now_ms).await?;
            let defaults = KernelDefaults {
                usage_limit_requests: limit.unwrap_or(old.usage_limit_requests),
                window_ms: window.unwrap_or(old.window_ms),
            };
            let (model, effort, next_runtime) =
                match defaults_runtime(body, &runtime, input.now_ms)? {
                    DefaultRuntime::Continue(values) => values,
                    DefaultRuntime::Break(response) => return Ok(response),
                };
            let response = response(&model, &effort, &defaults);
            if next_runtime.is_none() && limit.is_none() && window.is_none() {
                return Ok(AuthResponse::source_json(200, response));
            }
            let cutover_key = keys::kernel_cutover();
            let rows = [(&lk, lr.as_ref()), (&wk, wr.as_ref()), (&dk, dr.as_ref())];
            let guard = if window.is_some() && defaults.window_ms != old.window_ms {
                let Some(row) = self
                    .defaults_cutover(&cutover_key, input.now_ms, rows)
                    .await?
                else {
                    continue;
                };
                Some(row)
            } else {
                None
            };
            let revision = store::revision(self.host)?;
            let result = async {
                let mut checks = vec![
                    check(&rk, rr.as_ref()),
                    check(&lk, lr.as_ref()),
                    check(&wk, wr.as_ref()),
                    check(&dk, dr.as_ref()),
                ];
                let mut writes = Vec::new();
                if let Some(guard) = &guard {
                    checks.push(check(&cutover_key, Some(guard)));
                    writes.push(RecordMutation::Delete {
                        key: cutover_key.clone(),
                    });
                }
                if let Some(runtime) = next_runtime {
                    writes.push(RecordMutation::Put {
                        key: rk,
                        record: encode_source_control_record(&revision, &runtime, None)?,
                    });
                }
                if let Some(limit) = limit {
                    writes.push(put(lk, &limit, None, &revision)?);
                }
                if let Some(window) = window {
                    writes.push(put(wk, &window, None, &revision)?);
                }
                if limit.is_some() || window.is_some() {
                    writes.push(put(dk, &defaults, None, &revision)?);
                }
                commit(self.store, checks, writes).await
            }
            .await;
            match result {
                Ok(true) => return Ok(AuthResponse::source_json(200, response)),
                other => {
                    if let Some(guard) = &guard {
                        self.release_cutover(&cutover_key, guard).await?;
                    }
                    other?;
                }
            }
        }
        Err(conflict("Defaults were modified concurrently; retry"))
    }

    async fn defaults_cutover(
        &self,
        cutover_key: &RecordKey,
        now: i64,
        rows: [(&RecordKey, Option<&StoredRecord>); 3],
    ) -> Result<Option<StoredRecord>, GatewayError> {
        let [(lk, lr), (wk, wr), (dk, dr)] = rows;
        let current = self.store.read(cutover_key).await?;
        if store::value(current.as_ref(), now)?.is_some() {
            return Ok(None);
        }
        let revision = store::revision(self.host)?;
        let marker = json!({"v":2,"id":revision,"created_at_ms":now,"expires_at_ms":now+300000});
        if !commit(
            self.store,
            vec![
                check(cutover_key, current.as_ref()),
                check(lk, lr),
                check(wk, wr),
                check(dk, dr),
            ],
            vec![put(
                cutover_key.clone(),
                &marker,
                Some(now + 300000),
                &revision,
            )?],
        )
        .await?
        {
            return Ok(None);
        }
        let row =
            self.store.read(cutover_key).await?.ok_or_else(|| {
                store::corrupt("Kernel default-window cutover guard is unavailable")
            })?;
        if row.revision != revision {
            return Err(store::corrupt(
                "Kernel default-window cutover guard changed",
            ));
        }
        match self.live_default_reservations(now).await {
            Ok(false) => Ok(Some(row)),
            result => {
                self.release_cutover(cutover_key, &row).await?;
                Err(match result {
                    Ok(_) => conflict(
                        "Active Kernel quota reservations must settle before changing the default window",
                    ),
                    Err(error) => error,
                })
            }
        }
    }
}

fn validate_defaults_fields(body: &SourceJson) -> Result<Option<AuthResponse>, GatewayError> {
    let fields = parse::source_entries(body).ok_or_else(|| invalid("Invalid JSON body"))?;
    for (name, _) in fields {
        if ![
            "model",
            "reasoning_effort",
            "kernel_policy_limit_requests",
            "kernel_policy_window_ms",
        ]
        .iter()
        .any(|allowed| name.as_units().iter().copied().eq(allowed.encode_utf16()))
        {
            if let Ok(name) = name.try_utf8() {
                return Err(invalid(&format!("Unknown defaults field: {name}")));
            }
            let mut units = "Unknown defaults field: "
                .encode_utf16()
                .collect::<Vec<_>>();
            units.extend_from_slice(name.as_units());
            let error = gateway_schema::source_request::error::SourceRequestError::new(
                400,
                JsString16::from_units(units),
                "invalid_request_error",
                Some("invalid_request_error"),
                None,
            );
            return Ok(Some(AuthResponse::source_json(
                error.status,
                error.body().clone(),
            )));
        }
    }
    Ok(None)
}

type DefaultRuntime =
    std::ops::ControlFlow<AuthResponse, (ModelId, JsString16, Option<SourceJson>)>;

fn defaults_runtime(
    body: &SourceJson,
    runtime: &Option<SourceJson>,
    now: i64,
) -> Result<DefaultRuntime, GatewayError> {
    let (mut model, mut effort) = model_defaults(runtime.as_ref());
    if source_body::member(body, "model").is_none()
        && source_body::member(body, "reasoning_effort").is_none()
    {
        return Ok(DefaultRuntime::Continue((model, effort, None)));
    }
    if runtime.is_none() {
        return Err(GatewayError::unavailable(
            "Runtime configuration is unavailable",
            "server_error",
        ));
    }
    if let Some(raw) = source_body::member(body, "model") {
        model = parse::source_text(Some(&raw))
            .filter(|text| {
                !text
                    .as_units()
                    .iter()
                    .any(|unit| source_body::whitespace(*unit))
            })
            .map(ModelId::from_source)
            .ok_or_else(|| invalid("model must be a non-empty string"))?;
    }
    if model.as_js_string().as_units().is_empty() {
        return Err(invalid("model must be a non-empty string"));
    }
    let snapshot = runtime
        .as_ref()
        .and_then(|value| source_body::member(value, "codex_models"))
        .and_then(|value| value.deserialize::<ModelSnapshot>().ok())
        .filter(|value| !value.models.is_empty())
        .ok_or_else(|| conflict("No Codex model snapshot stored"))?;
    let record = snapshot
        .models
        .iter()
        .find(|record| {
            source_body::member(record, "slug")
                .as_ref()
                .and_then(source_body::text)
                .as_ref()
                == Some(model.as_js_string())
        })
        .ok_or_else(|| invalid("model is not in the stored Codex model list"))?;
    let reasoning = ModelReasoning::from_record(Some(record));
    effort = if let Some(raw) = source_body::member(body, "reasoning_effort") {
        parse::source_text(Some(&raw))
            .ok_or_else(|| invalid("reasoning_effort must be a non-empty string"))?
    } else {
        reasoning
            .default_level
            .or_else(|| reasoning.levels.first().cloned())
            .unwrap_or_else(|| JsString16::from_utf8("none"))
    };
    let next_runtime = Some(
        match build_runtime_config(&snapshot, Some(&model), Some(&effort), now) {
            Ok(value) => value,
            Err(error) => {
                // The original scalar error path remains unchanged;
                // only an actual source-reflected opaque message needs
                // the existing lossless source response rail.
                if SourceJson::from_serializable(&error.diagnostic().envelope)? == *error.body() {
                    return Err(error.diagnostic().clone());
                }
                return Ok(DefaultRuntime::Break(AuthResponse::source_json(
                    error.status,
                    error.body().clone(),
                )));
            }
        },
    );
    Ok(DefaultRuntime::Continue((model, effort, next_runtime)))
}

fn model_defaults(runtime: Option<&SourceJson>) -> (ModelId, JsString16) {
    (
        runtime
            .and_then(default_model)
            .unwrap_or_else(|| ModelId::from_scalar("")),
        runtime
            .and_then(default_reasoning_effort)
            .unwrap_or_else(|| JsString16::from_utf8("medium")),
    )
}
