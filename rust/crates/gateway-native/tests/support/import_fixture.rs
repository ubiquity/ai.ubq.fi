//! Fixed parameterless, compile-only debug harness, never an ordinary service.
#[cfg(unix)]
fn main() {
    gateway_native_import_actor::entry::run_native_import_fixture()
}
#[cfg(not(unix))]
fn main() {
    std::process::exit(78)
}
