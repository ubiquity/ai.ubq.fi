//! Private reverse CLI: stdin JSON only, one verified export archive into a NEW
//! private NativeCapture v2 directory. No network, dotenv, live database or restore.
#[cfg(all(unix, not(target_arch = "wasm32")))]
fn main() {
    use std::io::{Read, Write};
    let execute = || -> Result<String, String> {
        if std::env::args_os().len() != 1 {
            return Err("Private input is stdin only".into());
        }
        let mut raw = Vec::new();
        std::io::stdin()
            .lock()
            .take(65_537)
            .read_to_end(&mut raw)
            .map_err(|_| "Private input cannot be read".to_owned())?;
        if raw.len() > 65_536 {
            return Err("Private input exceeds its bound".into());
        }
        let input =
            serde_json::from_slice(&raw).map_err(|_| "Private input is invalid".to_owned())?;
        let receipt =
            gateway_state_reverse::reverse::execute(input).map_err(|error| error.to_string())?;
        serde_json::to_string(&receipt).map_err(|_| "Receipt cannot be encoded".to_owned())
    };
    match execute() {
        Ok(receipt) => {
            let mut stdout = std::io::stdout().lock();
            if stdout
                .write_all(receipt.as_bytes())
                .and_then(|()| stdout.write_all(b"\n"))
                .is_err()
            {
                std::process::exit(74);
            }
        }
        Err(message) => {
            eprintln!("State-transfer reverse refused: {message}");
            std::process::exit(78);
        }
    }
}

#[cfg(not(all(unix, not(target_arch = "wasm32"))))]
fn main() {
    eprintln!("State-transfer reverse requires native Unix");
    std::process::exit(78);
}
