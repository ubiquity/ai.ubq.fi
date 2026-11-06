//! Host-agnostic admin routes. The Worker authenticates the configured admin
//! token first, bounds the body, then passes the raw intent header and setting.
use crate::export::{ExportRequest, export_page};
use crate::import::{import, status};
use crate::intent::Intent;
use crate::refused;
use gateway_core::error::GatewayError;
use gateway_core::query::QueryStore;
use gateway_migration::staged::{STAGED_BATCH_MAX_BYTES, decode_stage_request};

pub const IMPORT_PATH: &str = "/admin/state-transfer/import";
pub const EXPORT_PATH: &str = "/admin/state-transfer/export";
/// One staged frame or one export cursor; the staged decoder repeats the bound.
pub const REQUEST_MAX_BYTES: usize = STAGED_BATCH_MAX_BYTES;
pub const EXPORT_SHA256_HEADER: &str = "x-uos-state-export-sha256";
pub const EXPORT_COUNT_HEADER: &str = "x-uos-state-export-count";
pub const EXPORT_CURSOR_HEADER: &str = "x-uos-state-export-next-cursor";

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Operation {
    ImportStatus,
    ImportFrame,
    ExportPage,
}

pub fn operation(method: &str, path: &str) -> Option<Operation> {
    match (method, path) {
        ("GET", IMPORT_PATH) => Some(Operation::ImportStatus),
        ("POST", IMPORT_PATH) => Some(Operation::ImportFrame),
        ("POST", EXPORT_PATH) => Some(Operation::ExportPage),
        _ => None,
    }
}

#[derive(Debug)]
pub struct Reply {
    pub status: u16,
    pub headers: Vec<(&'static str, String)>,
    pub body: Vec<u8>,
}

fn json<T: serde::Serialize>(value: &T) -> Result<Reply, GatewayError> {
    Ok(Reply {
        status: 200,
        headers: vec![("content-type", "application/json".into())],
        body: serde_json::to_vec(value).map_err(|_| refused("Reply cannot be encoded"))?,
    })
}

pub async fn handle<S: QueryStore>(
    store: &S,
    operation: Operation,
    configured: Option<&str>,
    presented: Option<&str>,
    body: &[u8],
) -> Result<Reply, GatewayError> {
    let intent = Intent::verify(presented, configured)?;
    if body.len() > REQUEST_MAX_BYTES {
        return Err(GatewayError::new(
            413,
            "State-transfer request exceeds its bound",
            Some("state_transfer_too_large"),
        ));
    }
    match operation {
        Operation::ImportStatus => {
            intent.import_capture()?;
            json(&status(store).await?)
        }
        Operation::ImportFrame => {
            let request = decode_stage_request(body)?;
            json(&import(store, &intent, request).await?)
        }
        Operation::ExportPage => {
            intent.require_export()?;
            let request: ExportRequest = serde_json::from_slice(body)
                .map_err(|_| refused("Export page request is invalid"))?;
            let page = export_page(store, request).await?;
            let mut headers = vec![
                ("content-type", "application/json".to_owned()),
                (EXPORT_SHA256_HEADER, page.sha256),
                (EXPORT_COUNT_HEADER, page.count.to_string()),
            ];
            if let Some(cursor) = page.next_cursor {
                headers.push((EXPORT_CURSOR_HEADER, cursor));
            }
            Ok(Reply {
                status: 200,
                headers,
                body: page.body,
            })
        }
    }
}
