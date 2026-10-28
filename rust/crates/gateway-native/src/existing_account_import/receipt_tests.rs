//! Synthetic-only private durable operation cases; no proof/claim/receipt leaves the admitted callback.
use super::{ExistingAccountImportStore, durability::DurableNativeImport};
use crate::{
    import_ingress::receive_import_connection,
    native_import::{NativeImportContext, NativeImportHandler},
    owner_control::NativeControlContext,
};
use gateway_auth_credentials::native_credentials::parse_native_document;
use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
use gateway_auth_native_contracts::native_import::NativeImportMetadata;
use gateway_auth_passkeys::passkeys;
use gateway_auth_primitives::crypto::sha256_hex;
use gateway_authority::native_control::import::AuthenticatedNativeImport;
use gateway_authority::native_control::import::effects::NativeImportClaim;
use gateway_authority::native_control::receiver::NativeRenewalAuthority;
use gateway_authority::store::Authority;
use gateway_authority_native_records::import_effects::NativeImportMode;
use gateway_authority_native_records::owner::RegisteredRenewalOwner;
use gateway_native_import_source::result::{NativeImportCompletion, NativeImportResult};
use gateway_native_import_test_support::{
    fixture::{Cancel, Fixture, Store, Tokens, fail, now_ms, run},
    receipt::{Home, document, fixture},
    socket::SocketRequest,
};
use gateway_source::{error::GatewayError, source_json::SourceJson};
use serde_json::json;
use std::{
    collections::BTreeMap,
    fs,
    os::unix::fs::PermissionsExt,
    sync::{
        Mutex,
        atomic::{AtomicBool, Ordering},
    },
};
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
        let native = self
            .native
            .lock()
            .map_err(|_| fail())?
            .take()
            .ok_or_else(fail)?;
        let mut claim = run(control.claim_import(proof))?;
        let start = run(control.start_import(&mut claim, proof))?.ok_or_else(fail)?;
        let receipt = native.issue_fixture(start, proof, raw, context, original, now_ms()?)?;
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
        pending()
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
            parse_native_document(next)
                .map_err(|_| fail())?
                .generation_hash()
        );
        let reference = receipt.receipt_ref();
        assert_eq!(reference.len(), 64);
        assert!(!reference.contains(&sha256_hex(raw)));
        drop(native);
        let reopened = ExistingAccountImportStore::configured(&f.owner, f.account.clone())
            .map_err(|_| fail())?;
        let recovered = run(reopened.reconcile(control, replay, proof, raw))?;
        assert_eq!(recovered.receipt_ref(), reference);
        run(reopened.recheck_receipt(control, replay, proof, raw, &recovered))?;
        let stored: serde_json::Value =
            serde_json::from_slice(&fs::read(home.path().join("auth.json")).map_err(|_| fail())?)
                .map_err(|_| fail())?;
        assert_eq!(stored["OPENAI_API_KEY"], old["OPENAI_API_KEY"]);
        assert_eq!(stored["other"], old["other"]);
        assert_eq!(stored["tokens"], next["tokens"]);
        assert_eq!(
            serde_json::from_slice::<serde_json::Value>(
                &fs::read(home.path().join(format!(
                    ".uos-import-{}.previous",
                    proof.metadata().action_ref
                )))
                .map_err(|_| fail())?
            )
            .map_err(|_| fail())?,
            **old
        );
        assert_eq!(
            fs::read(other.path().join("auth.json")).map_err(|_| fail())?,
            *other_before
        );
        let captured = f.store.captured()?;
        for secret in [
            "SYNTHETIC_REFRESH_old",
            "SYNTHETIC_REFRESH_new",
            "SYNTHETIC_INDEPENDENT_KEY",
            &sha256_hex(raw),
        ] {
            assert!(!captured.contains(secret));
        }
        let rows = f.store.rows.lock().map_err(|_| fail())?;
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
                        home.path().to_str().ok_or_else(fail)?.into(),
                    )
                    .map_err(|_| fail())?,
                    f.account.clone(),
                )
                .map_err(|_| fail())?;
                assert!(run(foreign.reconcile(control, replay, proof, &body)).is_err());
                return Ok(());
            }
            "receipt" => {
                let path = home.path().join(format!(
                    ".uos-import-{}.receipt",
                    proof.metadata().action_ref
                ));
                let mut wire: serde_json::Value =
                    serde_json::from_slice(&fs::read(&path).map_err(|_| fail())?)
                        .map_err(|_| fail())?;
                wire["operation_id"] = json!("foreign-operation");
                fs::write(path, serde_json::to_vec(&wire).map_err(|_| fail())?)
                    .map_err(|_| fail())?;
            }
            "account" => {
                let mut value = document("foreign")?;
                value["tokens"]["account_id"] = json!("foreign-account");
                home.write_auth(&value)?;
            }
            "symlink" => {
                let original = home.path().join("retained-auth.json");
                fs::rename(home.path().join("auth.json"), &original).map_err(|_| fail())?;
                std::os::unix::fs::symlink(original, home.path().join("auth.json"))
                    .map_err(|_| fail())?;
            }
            "generation" => home.write_auth(&document("advanced-without-retry")?)?,
            "actor" => {
                f.store
                    .rows
                    .lock()
                    .map_err(|_| fail())?
                    .get_mut(&passkeys::key("users", "user-fixture").0)
                    .ok_or_else(fail)?
                    .revision = "user-replaced".into()
            }
            "source_cf" => {
                f.store
                    .rows
                    .lock()
                    .map_err(|_| fail())?
                    .get_mut(&codex_source_pool_key().0)
                    .ok_or_else(fail)?
                    .revision = "source-replaced".into()
            }
            "cancel" => f.cancel.0.store(true, Ordering::SeqCst),
            _ => {
                let path = home.path().with_extension("retained");
                fs::rename(home.path(), &path).map_err(|_| fail())?;
                fs::create_dir(home.path()).map_err(|_| fail())?;
                fs::set_permissions(home.path(), fs::Permissions::from_mode(0o700))
                    .map_err(|_| fail())?;
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
            fs::remove_dir_all(path).map_err(|_| fail())?;
        }
        Ok(())
    }
}

fn pending() -> Result<NativeImportResult, GatewayError> {
    Ok(NativeImportResult {
        status: 202,
        headers: BTreeMap::new(),
        body: SourceJson::parse(r#"{"state":"synthetic_native_durable_publication_pending"}"#)
            .map_err(|_| fail())?,
        completion: NativeImportCompletion::PendingUnknown,
    })
}
fn receive(
    f: &Fixture,
    a: &Authority<Store, Tokens>,
    mut socket: SocketRequest,
    raw: &[u8],
    issuer: &Issuer<'_>,
) -> Result<Result<NativeImportResult, GatewayError>, GatewayError> {
    socket.send(raw)?;
    Ok(receive_import_connection(
        &mut socket.accepted,
        a,
        &f.identity,
        &f.owner,
        &f.config,
        &socket.nonces,
        issuer,
    ))
}
#[test]
fn durable_native_receipt_reopens_original_context_without_refresh_or_published()
-> Result<(), GatewayError> {
    let home = Home::fresh()?;
    let other = Home::fresh()?;
    let old = document("old")?;
    home.write_auth(&old)?;
    other.write_auth(&document("independent")?)?;
    let other_before = fs::read(other.path().join("auth.json")).map_err(|_| fail())?;
    let f = fixture(&home, &old)?;
    let a = f.authority();
    let next = document("new")?;
    let raw = serde_json::to_vec(&json!({"auth":next,"models":{"client_version":"0.100.0"}}))
        .map_err(|_| fail())?;
    let issuer = Issuer {
        f: &f,
        authority: &a,
        native: Mutex::new(Some(
            ExistingAccountImportStore::configured(&f.owner, f.account.clone())
                .map_err(|_| fail())?,
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
    let response = receive(
        &f,
        &a,
        SocketRequest::prepared(&f, &a, &raw)?,
        &raw,
        &issuer,
    )??;
    assert!(matches!(
        response.completion,
        NativeImportCompletion::PendingUnknown
    ));
    assert_eq!(response.status, 202);
    assert!(issuer.created.load(Ordering::SeqCst));
    Ok(())
}
#[test]
fn native_receipt_wrong_owner_source_account_body_and_replaced_files_refuse()
-> Result<(), GatewayError> {
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
        let home = Home::fresh()?;
        let old = document("old")?;
        home.write_auth(&old)?;
        let f = fixture(&home, &old)?;
        let a = f.authority();
        let raw = serde_json::to_vec(&document("new")?).map_err(|_| fail())?;
        let issuer = Issuer {
            f: &f,
            authority: &a,
            native: Mutex::new(Some(
                ExistingAccountImportStore::configured(&f.owner, f.account.clone())
                    .map_err(|_| fail())?,
            )),
            case: ReceiptCase::Refuse {
                changed,
                home: &home,
            },
            created: AtomicBool::new(false),
        };
        receive(
            &f,
            &a,
            SocketRequest::prepared(&f, &a, &raw)?,
            &raw,
            &issuer,
        )??;
        assert!(issuer.created.load(Ordering::SeqCst));
    }
    Ok(())
}
#[test]
fn production_mutation_is_unavailable_without_real_native_writer_exclusion()
-> Result<(), GatewayError> {
    let home = Home::fresh()?;
    let old = document("old")?;
    home.write_auth(&old)?;
    let hash = parse_native_document(&old)
        .map_err(|_| fail())?
        .routing_account_hash();
    let native = ExistingAccountImportStore::configured(
        &RegisteredRenewalOwner::configured(
            "native-owner".into(),
            home.path().to_str().ok_or_else(fail)?.into(),
        )
        .map_err(|_| fail())?,
        hash,
    )
    .map_err(|_| fail())?;
    let before = fs::read(home.path().join("auth.json")).map_err(|_| fail())?;
    let error = native.require_mutation_owner().err().ok_or_else(fail)?;
    assert_eq!(error.status, 503);
    assert_eq!(
        error.envelope.error.code.as_deref(),
        Some("native_import_owner_exclusion_unavailable")
    );
    assert_eq!(
        fs::read(home.path().join("auth.json")).map_err(|_| fail())?,
        before
    );
    assert_eq!(fs::read_dir(home.path()).map_err(|_| fail())?.count(), 1);
    Ok(())
}

#[test]
fn wrong_uploaded_account_owner_and_physical_source_write_nothing() -> Result<(), GatewayError> {
    for changed in ["account", "owner", "source"] {
        let home = Home::fresh()?;
        let old = document("old")?;
        home.write_auth(&old)?;
        let f = fixture(&home, &old)?;
        let a = f.authority();
        let foreign = RegisteredRenewalOwner::configured(
            "foreign-owner".into(),
            home.path().to_str().ok_or_else(fail)?.into(),
        )
        .map_err(|_| fail())?;
        let native = ExistingAccountImportStore::configured(
            if changed == "owner" {
                &foreign
            } else {
                &f.owner
            },
            f.account.clone(),
        )
        .map_err(|_| fail())?;
        let mut uploaded = document("new")?;
        if changed == "account" {
            uploaded["tokens"]["account_id"] = json!("foreign-account");
        }
        let raw = serde_json::to_vec(&uploaded).map_err(|_| fail())?;
        let socket = SocketRequest::prepared(&f, &a, &raw)?;
        if changed == "source" {
            home.write_auth(&document("independent-native-advance")?)?;
        }
        let before = fs::read(home.path().join("auth.json")).map_err(|_| fail())?;
        let issuer = Issuer {
            f: &f,
            authority: &a,
            native: Mutex::new(Some(native)),
            case: ReceiptCase::NoWrite,
            created: AtomicBool::new(false),
        };
        let refusal = receive(&f, &a, socket, &raw, &issuer)?
            .err()
            .ok_or_else(fail)?;
        assert_ne!(
            refusal.envelope.error.code.as_deref(),
            Some("fixture_refusal"),
            "setup failure is not native refusal"
        );
        assert_eq!(
            fs::read(home.path().join("auth.json")).map_err(|_| fail())?,
            before,
            "{changed}"
        );
        assert_eq!(
            fs::read_dir(home.path()).map_err(|_| fail())?.count(),
            1,
            "{changed}"
        );
        assert!(!issuer.created.load(Ordering::SeqCst));
        let rows = f.store.rows.lock().map_err(|_| fail())?;
        assert!(rows.values().all(|row| row.value["phase"] != "published"));
    }
    Ok(())
}
