#[cfg(unix)]
fn main() {
    gateway_native_import_actor::entry::run_native_import()
}
#[cfg(not(unix))]
fn main() {
    std::process::exit(78)
}
