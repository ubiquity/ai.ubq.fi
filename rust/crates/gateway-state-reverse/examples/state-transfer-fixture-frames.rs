//! Synthetic conformance input only: the committed native fixture as ordered
//! stage frames on stdout. No arguments, credential, database or network.
#[cfg(all(unix, not(target_arch = "wasm32")))]
fn main() {
    use gateway_migration::native_capture::{NativeCapture, prepare_native};
    use gateway_migration::staged::StagePlan;
    use std::io::Write;
    let frames = || -> Result<Vec<u8>, String> {
        if std::env::args_os().len() != 1 {
            return Err("No arguments are accepted".into());
        }
        let text = include_str!("../../gateway-migration/tests/fixtures/native-source.json");
        let capture: NativeCapture = serde_json::from_str(text).map_err(|e| e.to_string())?;
        let prepared = prepare_native(&capture).map_err(|e| e.to_string())?;
        let mut plan = StagePlan::from_prepared(prepared).map_err(|e| format!("{e:?}"))?;
        let mut output = Vec::new();
        let mut push = |frame: &gateway_migration::staged::StageRequest| -> Result<(), String> {
            serde_json::to_writer(&mut output, frame).map_err(|e| e.to_string())?;
            output.push(b'\n');
            Ok(())
        };
        push(&plan.begin())?;
        while let Some(batch) = plan.next_batch().map_err(|e| format!("{e:?}"))? {
            push(&batch)?;
        }
        push(&plan.seal())?;
        Ok(output)
    };
    match frames() {
        Ok(bytes) => {
            if std::io::stdout().lock().write_all(&bytes).is_err() {
                std::process::exit(74);
            }
        }
        Err(message) => {
            eprintln!("Fixture frames refused: {message}");
            std::process::exit(78);
        }
    }
}

#[cfg(not(all(unix, not(target_arch = "wasm32"))))]
fn main() {
    eprintln!("Fixture frames require native Unix");
    std::process::exit(78);
}
