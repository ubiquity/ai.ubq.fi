#[cfg(unix)]
fn main() {
    gateway_native_connector::connector_worker::run_native_connector();
}
#[cfg(not(unix))]
fn main() {
    std::process::exit(78);
}
