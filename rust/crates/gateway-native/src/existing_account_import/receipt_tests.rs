//! Synthetic-only private durable operation cases; no proof/claim/receipt leaves the admitted callback.
use super::{ExistingAccountImportStore, durability::DurableNativeImport};
use crate::{
    import_ingress::receive_import_connection,
    native_import::{
        NativeImportCompletion, NativeImportContext, NativeImportHandler, NativeImportResult,
    },
    owner_control::NativeControlContext,
};
use gateway_auth::{
    crypto::{base64url, sha256_hex},
    native_import::NativeImportMetadata,
    passkeys,
    provider_credentials::codex_source_pool_key,
};
use gateway_authority::{
    native_control::{
        import::AuthenticatedNativeImport,
        import::effects::{NativeImportClaim, NativeImportMode},
        receiver::{NativeRenewalAuthority, RegisteredRenewalOwner},
    },
    store::Authority,
};
use gateway_core::{error::GatewayError, source_json::SourceJson};
use gateway_native_import_test_support::{
    fixture::{Cancel, Fixture, Store, Tokens, fail, now_ms, run},
    socket::SocketRequest,
};
use serde_json::json;
use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    os::unix::fs::{OpenOptionsExt, PermissionsExt},
    path::PathBuf,
    sync::{
        Mutex,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
};
struct Home(PathBuf);
impl Home {
    fn fresh() -> Self {
        static SERIAL: AtomicU64 = AtomicU64::new(0);
        let root = fs::canonicalize(std::env::temp_dir()).unwrap();
        assert!(root.is_dir());
        let path = root.join(format!(
            "native-import-receipt-{}-{}",
            std::process::id(),
            SERIAL.fetch_add(1, Ordering::SeqCst)
        ));
        fs::create_dir(&path).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700)).unwrap();
        Self(path)
    }
    fn write_auth(&self, value: &serde_json::Value) {
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .mode(0o600)
            .open(self.0.join("auth.json"))
            .unwrap();
        file.write_all(&serde_json::to_vec(value).unwrap()).unwrap();
        file.sync_all().unwrap();
    }
}
impl Drop for Home {
    fn drop(&mut self) {
        fs::remove_dir_all(&self.0).unwrap();
    }
}
fn document(label: &str) -> serde_json::Value {
    let access = format!(
        "fixture.{}.signature",
        base64url(
            serde_json::to_string(&json!({"exp":now_ms()/1000+3600,"label":label}))
                .unwrap()
                .as_bytes()
        )
    );
    json!({"tokens":{"account_id":"synthetic-account","access_token":access,"refresh_token":format!("SYNTHETIC_REFRESH_{label}")},"OPENAI_API_KEY":"SYNTHETIC_INDEPENDENT_KEY","other":{"independent":label}})
}
fn fixture(home: &Home, old: &serde_json::Value) -> Fixture {
    let mut f = Fixture::new();
    let credentials = gateway_auth::native_credentials::parse_native_document(old).unwrap();
    let key = codex_source_pool_key();
    let mut source = {
        let rows = f.store.rows.lock().unwrap();
        gateway_auth::provider_credentials::parse_codex_source_pool(
            &rows.get(&key.0).unwrap().value,
        )
        .unwrap()
        .unwrap()
    };
    let native = source.accounts[0].native_owner.as_mut().unwrap();
    native.codex_home = home.0.to_str().unwrap().into();
    native.managed_source.as_mut().unwrap().generation_hash = credentials.generation_hash();
    f.store.seed(key, "source-1", source.private_source_value());
    f.owner =
        RegisteredRenewalOwner::configured("native-owner".into(), home.0.to_str().unwrap().into())
            .unwrap();
    f
}

enum ReceiptCase<'a> {
    Recover {
        home: &'a Home,
        other: &'a Home,
        old: &'a serde_json::Value,
        next: &'a serde_json::Value,
        other_before: &'a [u8],
    },
    Refuse {
        changed: &'a str,
        home: &'a Home,
    },
    NoWrite,
}
struct Issuer<'a> {
    f: &'a Fixture,
    authority: &'a Authority<Store, Tokens>,
    native: Mutex<Option<ExistingAccountImportStore>>,
    case: ReceiptCase<'a>,
    created: AtomicBool,
}
impl NativeImportHandler for Issuer<'_> {
    fn import_document(
        &self,
        proof: &AuthenticatedNativeImport,
        metadata: &NativeImportMetadata,
        raw: &[u8],
        context: &NativeImportContext<'_>,
        original: &NativeControlContext<'_>,
    ) -> Result<NativeImportResult, GatewayError> {
        context.require_original(original)?;
        context.check_document(proof, metadata, raw)?;
        let f = self.f;
        let control = NativeRenewalAuthority::configured(
            self.authority,
            &f.identity,
            &f.owner,
            &f.config,
            &f.cancel,
        );
        run(context.check_current(&control))?;
        let native = self.native.lock().unwrap().take().unwrap();
        let mut claim = run(control.claim_import(proof))?;
        let start = run(control.start_import(&mut claim, proof))?.ok_or_else(fail)?;
        let receipt = native.issue_fixture(start, proof, raw, context, original, now_ms())?;
        self.created.store(true, Ordering::SeqCst);
        assert!(
            !matches!(self.case, ReceiptCase::NoWrite),
            "invalid input reached native write"
        );
        run(control.unknown_import(&claim))?;
        let mut replay = run(control.claim_import(proof))?;
        assert_eq!(replay.mode(), NativeImportMode::ReconcileOnly);
        assert!(run(control.start_import(&mut replay, proof))?.is_none());
        match &self.case {
            ReceiptCase::Recover { .. } => {
                self.verify_recovery(&control, native, &replay, proof, raw, receipt)?
            }
            ReceiptCase::Refuse { .. } => {
                self.verify_refusal(&control, native, &replay, proof, raw, receipt)?
            }
            ReceiptCase::NoWrite => unreachable!(),
        }
        Ok(pending())
    }
}
impl Issuer<'_> {
    fn verify_recovery(
        &self,
        control: &NativeRenewalAuthority<'_, Store, Tokens, Cancel>,
        native: ExistingAccountImportStore,
        replay: &NativeImportClaim,
        proof: &AuthenticatedNativeImport,
        raw: &[u8],
        receipt: DurableNativeImport,
    ) -> Result<(), GatewayError> {
        let ReceiptCase::Recover {
            home,
            other,
            old,
            next,
            other_before,
        } = &self.case
        else {
            unreachable!()
        };
        let f = self.f;
        run(native.recheck_receipt(control, replay, proof, raw, &receipt))?;
        assert_eq!(receipt.account_id_hash(), f.account);
        assert_eq!(
            receipt.native_generation(),
            gateway_auth::native_credentials::parse_native_document(next)
                .unwrap()
                .generation_hash()
        );
        let reference = receipt.receipt_ref();
        assert_eq!(reference.len(), 64);
        assert!(!reference.contains(&sha256_hex(raw)));
        drop(native);
        let reopened = ExistingAccountImportStore::configured(&f.owner, f.account.clone()).unwrap();
        let recovered = run(reopened.reconcile(control, replay, proof, raw))?;
        assert_eq!(recovered.receipt_ref(), reference);
        run(reopened.recheck_receipt(control, replay, proof, raw, &recovered))?;
        let stored: serde_json::Value =
            serde_json::from_slice(&fs::read(home.0.join("auth.json")).unwrap()).unwrap();
        assert_eq!(stored["OPENAI_API_KEY"], old["OPENAI_API_KEY"]);
        assert_eq!(stored["other"], old["other"]);
        assert_eq!(stored["tokens"], next["tokens"]);
        assert_eq!(
            serde_json::from_slice::<serde_json::Value>(
                &fs::read(home.0.join(format!(
                    ".uos-import-{}.previous",
                    proof.metadata().action_ref
                )))
                .unwrap()
            )
            .unwrap(),
            **old
        );
        assert_eq!(fs::read(other.0.join("auth.json")).unwrap(), *other_before);
        let captured = f.store.captured();
        for secret in [
            "SYNTHETIC_REFRESH_old",
            "SYNTHETIC_REFRESH_new",
            "SYNTHETIC_INDEPENDENT_KEY",
            &sha256_hex(raw),
        ] {
            assert!(!captured.contains(secret));
        }
        let rows = f.store.rows.lock().unwrap();
        let operations = rows
            .values()
            .filter(|r| r.value.get("phase").is_some())
            .collect::<Vec<_>>();
        assert_eq!(operations.len(), 1);
        assert_eq!(operations[0].value["phase"], "unknown");
        assert!(operations[0].value["terminal"].is_null());
        Ok(())
    }
    fn verify_refusal(
        &self,
        control: &NativeRenewalAuthority<'_, Store, Tokens, Cancel>,
        native: ExistingAccountImportStore,
        replay: &NativeImportClaim,
        proof: &AuthenticatedNativeImport,
        raw: &[u8],
        receipt: DurableNativeImport,
    ) -> Result<(), GatewayError> {
        let ReceiptCase::Refuse { changed, home } = &self.case else {
            unreachable!()
        };
        let f = self.f;
        let mut body = raw.to_vec();
        let mut moved = None;
        match *changed {
            "body" => body.push(b' '),
            "owner" => {
                let foreign = ExistingAccountImportStore::configured(
                    &RegisteredRenewalOwner::configured(
                        "foreign-owner".into(),
                        home.0.to_str().unwrap().into(),
                    )
                    .unwrap(),
                    f.account.clone(),
                )
                .unwrap();
                assert!(run(foreign.reconcile(control, replay, proof, &body)).is_err());
                return Ok(());
            }
            "receipt" => {
                let path = home.0.join(format!(
                    ".uos-import-{}.receipt",
                    proof.metadata().action_ref
                ));
                let mut wire: serde_json::Value =
                    serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
                wire["operation_id"] = json!("foreign-operation");
                fs::write(path, serde_json::to_vec(&wire).unwrap()).unwrap();
            }
            "account" => {
                let mut value = document("foreign");
                value["tokens"]["account_id"] = json!("foreign-account");
                home.write_auth(&value);
            }
            "symlink" => {
                let original = home.0.join("retained-auth.json");
                fs::rename(home.0.join("auth.json"), &original).unwrap();
                std::os::unix::fs::symlink(original, home.0.join("auth.json")).unwrap();
            }
            "generation" => home.write_auth(&document("advanced-without-retry")),
            "actor" => {
                f.store
                    .rows
                    .lock()
                    .unwrap()
                    .get_mut(&passkeys::key("users", "user-fixture").0)
                    .unwrap()
                    .revision = "user-replaced".into()
            }
            "source_cf" => {
                f.store
                    .rows
                    .lock()
                    .unwrap()
                    .get_mut(&codex_source_pool_key().0)
                    .unwrap()
                    .revision = "source-replaced".into()
            }
            "cancel" => f.cancel.0.store(true, Ordering::SeqCst),
            _ => {
                let path = home.0.with_extension("retained");
                fs::rename(&home.0, &path).unwrap();
                fs::create_dir(&home.0).unwrap();
                fs::set_permissions(&home.0, fs::Permissions::from_mode(0o700)).unwrap();
                moved = Some(path);
            }
        }
        assert!(
            run(native.reconcile(control, replay, proof, &body)).is_err(),
            "{changed}"
        );
        if *changed != "body" {
            assert!(
                run(native.recheck_receipt(control, replay, proof, raw, &receipt)).is_err(),
                "{changed}"
            );
        }
        if let Some(path) = moved {
            fs::remove_dir_all(path).unwrap();
        }
        Ok(())
    }
}

fn pending() -> NativeImportResult {
    NativeImportResult {
        status: 202,
        headers: BTreeMap::new(),
        body: SourceJson::parse(r#"{"state":"synthetic_native_durable_publication_pending"}"#)
            .unwrap(),
        completion: NativeImportCompletion::PendingUnknown,
    }
}
fn receive(
    f: &Fixture,
    a: &Authority<Store, Tokens>,
    mut socket: SocketRequest,
    raw: &[u8],
    issuer: &Issuer<'_>,
) -> Result<NativeImportResult, GatewayError> {
    socket.send(raw);
    receive_import_connection(
        &mut socket.accepted,
        a,
        &f.identity,
        &f.owner,
        &f.config,
        &socket.nonces,
        issuer,
    )
}
#[test]
fn durable_native_receipt_reopens_original_context_without_refresh_or_published() {
    let home = Home::fresh();
    let other = Home::fresh();
    let old = document("old");
    home.write_auth(&old);
    other.write_auth(&document("independent"));
    let other_before = fs::read(other.0.join("auth.json")).unwrap();
    let f = fixture(&home, &old);
    let a = f.authority();
    let next = document("new");
    let raw =
        serde_json::to_vec(&json!({"auth":next,"models":{"client_version":"0.100.0"}})).unwrap();
    let issuer = Issuer {
        f: &f,
        authority: &a,
        native: Mutex::new(Some(
            ExistingAccountImportStore::configured(&f.owner, f.account.clone()).unwrap(),
        )),
        case: ReceiptCase::Recover {
            home: &home,
            other: &other,
            old: &old,
            next: &next,
            other_before: &other_before,
        },
        created: AtomicBool::new(false),
    };
    let response = receive(&f, &a, SocketRequest::prepared(&f, &a, &raw), &raw, &issuer).unwrap();
    assert!(matches!(
        response.completion,
        NativeImportCompletion::PendingUnknown
    ));
    assert_eq!(response.status, 202);
    assert!(issuer.created.load(Ordering::SeqCst));
}
#[test]
fn native_receipt_wrong_owner_source_account_body_and_replaced_files_refuse() {
    for changed in [
        "body",
        "owner",
        "receipt",
        "account",
        "symlink",
        "generation",
        "home",
        "actor",
        "source_cf",
        "cancel",
    ] {
        let home = Home::fresh();
        let old = document("old");
        home.write_auth(&old);
        let f = fixture(&home, &old);
        let a = f.authority();
        let raw = serde_json::to_vec(&document("new")).unwrap();
        let issuer = Issuer {
            f: &f,
            authority: &a,
            native: Mutex::new(Some(
                ExistingAccountImportStore::configured(&f.owner, f.account.clone()).unwrap(),
            )),
            case: ReceiptCase::Refuse {
                changed,
                home: &home,
            },
            created: AtomicBool::new(false),
        };
        receive(&f, &a, SocketRequest::prepared(&f, &a, &raw), &raw, &issuer).unwrap();
        assert!(issuer.created.load(Ordering::SeqCst));
    }
}
#[test]
fn production_mutation_is_unavailable_without_real_native_writer_exclusion() {
    let home = Home::fresh();
    let old = document("old");
    home.write_auth(&old);
    let hash = gateway_auth::native_credentials::parse_native_document(&old)
        .unwrap()
        .routing_account_hash();
    let native = ExistingAccountImportStore::configured(
        &RegisteredRenewalOwner::configured("native-owner".into(), home.0.to_str().unwrap().into())
            .unwrap(),
        hash,
    )
    .unwrap();
    let before = fs::read(home.0.join("auth.json")).unwrap();
    let error = native.require_mutation_owner().unwrap_err();
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_import_owner_exclusion_unavailable")
    );
    assert_eq!(fs::read(home.0.join("auth.json")).unwrap(), before);
    assert_eq!(fs::read_dir(&home.0).unwrap().count(), 1);
}

#[test]
fn wrong_uploaded_account_owner_and_physical_source_write_nothing() {
    for changed in ["account", "owner", "source"] {
        let home = Home::fresh();
        let old = document("old");
        home.write_auth(&old);
        let f = fixture(&home, &old);
        let a = f.authority();
        let foreign = RegisteredRenewalOwner::configured(
            "foreign-owner".into(),
            home.0.to_str().unwrap().into(),
        )
        .unwrap();
        let native = ExistingAccountImportStore::configured(
            if changed == "owner" {
                &foreign
            } else {
                &f.owner
            },
            f.account.clone(),
        )
        .unwrap();
        let mut uploaded = document("new");
        if changed == "account" {
            uploaded["tokens"]["account_id"] = json!("foreign-account");
        }
        let raw = serde_json::to_vec(&uploaded).unwrap();
        let socket = SocketRequest::prepared(&f, &a, &raw);
        if changed == "source" {
            home.write_auth(&document("independent-native-advance"));
        }
        let before = fs::read(home.0.join("auth.json")).unwrap();
        let issuer = Issuer {
            f: &f,
            authority: &a,
            native: Mutex::new(Some(native)),
            case: ReceiptCase::NoWrite,
            created: AtomicBool::new(false),
        };
        assert!(receive(&f, &a, socket, &raw, &issuer).is_err(), "{changed}");
        assert_eq!(
            fs::read(home.0.join("auth.json")).unwrap(),
            before,
            "{changed}"
        );
        assert_eq!(fs::read_dir(&home.0).unwrap().count(), 1, "{changed}");
        assert!(!issuer.created.load(Ordering::SeqCst));
        let rows = f.store.rows.lock().unwrap();
        assert!(rows.values().all(|row| row.value["phase"] != "published"));
    }
}
