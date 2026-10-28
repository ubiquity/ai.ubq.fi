//! Synthetic-only stdin/stdout controller; no network, child process, or live owner access.
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct Initialize {
    v: u32,
    directory: String,
    now_ms: i64,
    capture: gateway_migration_data::native_capture::NativeCapture,
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct Export {
    v: u32,
    now_ms: i64,
    artifacts: gateway_migration_protocol::operational::ArtifactInput,
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Serialize)]
struct Candidate {
    operation: &'static str,
    capture: gateway_migration_data::native_capture::NativeCapture,
    adoption_receipt: gateway_migration_prepare::engine::AdoptionReceipt,
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Deserialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
enum Phase {
    ExerciseRuntime {
        v: u32,
    },
    AdvanceManagedA {
        v: u32,
        artifacts: gateway_migration_protocol::operational::ArtifactInput,
    },
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Serialize)]
struct Inverse {
    operation: &'static str,
    source: gateway_migration_data::native_capture::NativeCapture,
    rust_controls: Vec<gateway_migration_data::custody::CustodyRecord>,
    artifacts: gateway_migration_data::custody::ArtifactContext,
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Deserialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
enum Preparation {
    PrepareInputs {
        v: u32,
        directory: String,
        now_ms: i64,
    },
    StagedCase {
        v: u32,
        directory: String,
        now_ms: i64,
    },
    StagedPrepareOnly {
        v: u32,
        directory: String,
        now_ms: i64,
    },
}
#[cfg(not(target_arch = "wasm32"))]
#[derive(serde::Deserialize)]
struct Selector {
    #[serde(default)]
    operation: Option<String>,
}
#[cfg(not(target_arch = "wasm32"))]
fn main() -> Result<(), gateway_migration_data::error::MigrationError> {
    use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
    use gateway_core::state::AuthorityStore;
    use gateway_migration_data::custody::ArtifactContext;
    use gateway_migration_fixture_custody::{host, staged};
    use gateway_migration_fixture_inputs::{inputs, producers};
    use gateway_migration_fixture_port::{executor::support_run, port};
    use gateway_migration_native_custody::native_custody::export_native_rollback_with_custody;
    use gateway_migration_native_custody::native_custody::sanitize_native_capture;
    use gateway_migration_prepare::engine::adopt_empty;
    use gateway_migration_prepare::engine::prepare_native;
    assert_eq!(
        std::env::args_os().len(),
        1,
        "The fixture accepts no source, credential, endpoint or target arguments"
    );
    // Inspect only the finite operation selector, then deserialize the ORIGINAL
    // raw document. Untagged serde buffering must not erase NativeCapture SourceJson.
    let initial: gateway_source::source_json::SourceJson = port::read()?;
    let selector: Selector = initial.deserialize()?;
    let init: Initialize = if selector.operation.is_none() {
        initial.deserialize()?
    } else {
        match initial.deserialize::<Preparation>()? {
            Preparation::StagedCase {
                v: 1,
                directory,
                now_ms,
            } => {
                return staged::run(&directory, now_ms, false);
            }
            Preparation::StagedPrepareOnly {
                v: 1,
                directory,
                now_ms,
            } => {
                return staged::run(&directory, now_ms, true);
            }
            Preparation::PrepareInputs {
                v: 1,
                directory,
                now_ms,
            } => {
                port::write(&inputs::prepare(&directory, now_ms)?)?;
                return Ok(());
            }
            _ => return Err(port::error("Unsupported fixture preparation").into()),
        }
    };
    if init.v != 1 {
        return Err(port::error("Unsupported fixture initialization").into());
    }
    let time = inputs::FixtureTime::new(init.capture.captured_at_ms)?;
    if init.now_ms != time.runtime() {
        return Err(port::error("Fixture initialization time differs").into());
    }
    time.check()?;
    let store = port::Store::default();
    let fixed = host::Host::from_stopped_fixture(&init.directory, &init.capture, init.now_ms)?;
    let candidate = support_run(sanitize_native_capture(&init.capture, &fixed))?;
    let prepared = prepare_native(&candidate)?;
    let adoption_receipt = support_run(adopt_empty(&store, &prepared))?;
    time.check()?;
    port::write(&Candidate {
        operation: "candidate",
        capture: candidate,
        adoption_receipt,
    })?;
    match port::read::<Phase>()? {
        Phase::ExerciseRuntime { v: 1 } => {}
        _ => return Err(port::error("Expected the fixed runtime producer phase").into()),
    }
    time.check()?;
    let runtime = producers::exercise(&store, time)?;
    time.check()?;
    port::write(&runtime)?;
    let artifacts: ArtifactContext = match port::read::<Phase>()? {
        Phase::AdvanceManagedA { v: 1, artifacts } => artifacts.into(),
        _ => return Err(port::error("Expected the fixed managed-a producer phase").into()),
    };
    time.check()?;
    let (fixed, receipt) = fixed.advance_managed_a(&store, time, &artifacts)?;
    time.check()?;
    port::write(&receipt)?;
    let export: Export = port::read()?;
    if export.v != 1 || export.now_ms != time.export() {
        return Err(port::error("Unsupported fixture export").into());
    }
    time.check()?;
    let current = support_run(store.read(&codex_source_pool_key()))?
        .ok_or_else(|| port::error("Current fixture source pool is missing"))?;
    let fixed = fixed.with_current_pool(&current, export.now_ms)?;
    let bundle = support_run(export_native_rollback_with_custody(
        &store,
        export.now_ms,
        &fixed,
        export.artifacts.into(),
    ))?;
    let (source, rust_controls, artifacts) = bundle.into_private_parts();
    time.check()?;
    port::write(&Inverse {
        operation: "inverse",
        source,
        rust_controls,
        artifacts,
    })?;
    Ok(())
}
#[cfg(target_arch = "wasm32")]
fn main() {}
