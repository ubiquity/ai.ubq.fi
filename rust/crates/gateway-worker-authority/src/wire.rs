//! Closed private binding protocol; never forwarded from public requests.
use gateway_core::{
    query::QuerySelector,
    state::{RecordKey, StoredRecord},
};
use serde::{Deserialize, Serialize};
pub const WIRE_VERSION_HEADER: &str = "x-uos-authority-wire-version";
pub const WIRE_VERSION: &str = "2";
pub const WIRE_URL: &str = "https://authority.internal/v1/store";
pub const WIRE_MAX_BYTES: usize = 32 * 1024 * 1024;
pub const AUTHORITY_OBJECT: &str = "gateway-authority-v1";
#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Envelope {
    pub access: gateway_authority_lifecycle::model::Access,
    pub request: StoreRequest,
}
#[derive(Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum StoreRequest {
    Coordination {
        request: gateway_authority_lifecycle::model::Operation,
    },
    ProfileMigration {
        request: ProfileMigration,
        request_id: String,
        now_ms: i64,
    },
    Read {
        key: String,
    },
    Commit {
        checks: Vec<WireCheck>,
        mutations: Vec<WireMutation>,
    },
    Query {
        selector: WireSelector,
        reverse: bool,
        limit: u32,
        cursor: Option<String>,
        now_ms: i64,
    },
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum ProfileMigration {
    Import {
        ndjson: String,
        profile: ProfileKind,
        include_cache: bool,
        include_legacy: bool,
        overwrite: bool,
        dry_run: bool,
    },
    Validate,
}
#[derive(Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ProfileKind {
    Local,
    Prod,
}
impl From<gateway_admin_runtime::capabilities::MigrationRequest> for ProfileMigration {
    fn from(request: gateway_admin_runtime::capabilities::MigrationRequest) -> Self {
        use gateway_admin_runtime::capabilities::{MigrationProfile, MigrationRequest};
        match request {
            MigrationRequest::Validate => Self::Validate,
            MigrationRequest::Import(value) => Self::Import {
                ndjson: value.ndjson,
                profile: match value.profile {
                    MigrationProfile::Local => ProfileKind::Local,
                    MigrationProfile::Prod => ProfileKind::Prod,
                },
                include_cache: value.include_cache,
                include_legacy: value.include_legacy,
                overwrite: value.overwrite,
                dry_run: value.dry_run,
            },
        }
    }
}
impl From<ProfileMigration> for gateway_admin_runtime::capabilities::MigrationRequest {
    fn from(request: ProfileMigration) -> Self {
        use gateway_admin_runtime::capabilities::{
            MigrationImport, MigrationProfile, MigrationRequest,
        };
        match request {
            ProfileMigration::Validate => MigrationRequest::Validate,
            ProfileMigration::Import {
                ndjson,
                profile,
                include_cache,
                include_legacy,
                overwrite,
                dry_run,
            } => MigrationRequest::Import(MigrationImport {
                ndjson,
                profile: match profile {
                    ProfileKind::Local => MigrationProfile::Local,
                    ProfileKind::Prod => MigrationProfile::Prod,
                },
                include_cache,
                include_legacy,
                overwrite,
                dry_run,
            }),
        }
    }
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case", deny_unknown_fields)]
pub enum WireSelector {
    Prefix {
        prefix: String,
        start: Option<String>,
        end: Option<String>,
    },
    Range {
        start: String,
        end: String,
    },
}

impl From<QuerySelector> for WireSelector {
    fn from(value: QuerySelector) -> Self {
        match value {
            QuerySelector::Prefix { prefix, start, end } => Self::Prefix {
                prefix: prefix.0,
                start: start.map(|key| key.0),
                end: end.map(|key| key.0),
            },
            QuerySelector::Range { start, end } => Self::Range {
                start: start.0,
                end: end.0,
            },
        }
    }
}

impl From<WireSelector> for QuerySelector {
    fn from(value: WireSelector) -> Self {
        match value {
            WireSelector::Prefix { prefix, start, end } => Self::Prefix {
                prefix: RecordKey(prefix),
                start: start.map(RecordKey),
                end: end.map(RecordKey),
            },
            WireSelector::Range { start, end } => Self::Range {
                start: RecordKey(start),
                end: RecordKey(end),
            },
        }
    }
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct WireEntry {
    pub key: String,
    pub record: StoredRecord,
}

#[derive(Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct WireCheck {
    pub key: String,
    pub expected_revision: Option<String>,
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum WireMutation {
    Put { key: String, record: StoredRecord },
    Delete { key: String },
}

#[derive(Deserialize, Serialize)]
#[serde(tag = "operation", rename_all = "snake_case", deny_unknown_fields)]
pub enum StoreResponse {
    Coordination {
        reply: Box<gateway_authority_lifecycle::model::Reply>,
    },
    ProfileMigration {
        status: u16,
        headers: std::collections::BTreeMap<String, String>,
        body: gateway_core::source_json::SourceJson,
    },
    Read {
        record: Option<StoredRecord>,
    },
    Commit {
        committed: bool,
    },
    Query {
        entries: Vec<WireEntry>,
        next_cursor: Option<String>,
    },
}
