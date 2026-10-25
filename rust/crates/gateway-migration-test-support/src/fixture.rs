use serde::Serialize;
use std::{
    future::Future,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    task::{Context, Poll, Waker},
};

pub struct Directory(PathBuf);
static NEXT_DIRECTORY: AtomicU64 = AtomicU64::new(1);
impl Default for Directory {
    fn default() -> Self {
        Self::new()
    }
}
impl Directory {
    pub fn new() -> Self {
        let suffix = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let nonce = NEXT_DIRECTORY.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "uos-migration-{}-{suffix}-{nonce}",
            std::process::id()
        ));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
    pub fn file(&self, name: &str) -> PathBuf {
        self.0.join(name)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}
pub fn run<F: Future>(future: F) -> F::Output {
    let mut cx = Context::from_waker(Waker::noop());
    let mut future = std::pin::pin!(future);
    loop {
        if let Poll::Ready(value) = future.as_mut().poll(&mut cx) {
            return value;
        }
        std::thread::yield_now();
    }
}

#[derive(Serialize)]
pub struct Receipt {
    pub operation: &'static str,
    pub cases: Vec<&'static str>,
    pub changed_keys: Vec<String>,
    pub deleted_keys: Vec<String>,
}

pub const RUNTIME_CASES: [&str; 5] = [
    "actual_adopted_api_authentication",
    "actual_post_restart_admission",
    "actual_admin_read_write",
    "actual_adopted_row_delete",
    "actual_latest_shared_graph_write",
];

pub const MANAGED_A_CASES: [&str; 4] = [
    "stale_owner_generation_refused",
    "actual_managed_a_current_projection",
    "unchanged_managed_b_physical_and_projection",
    "current_owner_inverse_accepted",
];
