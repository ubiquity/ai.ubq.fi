#![cfg(all(target_os = "macos", feature = "native-import-fixture"))]
mod existing_account_import {
    mod flow_tests {
        use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
        use gateway_auth_primitives::crypto::sha256_hex;
        use gateway_authority_native_records::owner::RegisteredRenewalOwner;
        use gateway_core::state::AuthorityStore;
        use gateway_native_import_flow_authority::fixture::{
            NEW, OLD, PauseHandle, Servers, setup,
        };
        use gateway_native_import_flow_process::fixture::{
            CaseRoot, assert_writer_stopped, no_native_effect, no_publication, original_request,
            request, revoke_admin, start_custodian, wait_failure,
        };
        use gateway_native_import_store::store::ExistingAccountImportStore;
        use gateway_native_import_test_support::fixture as base;
        use gateway_source::error::GatewayError;
        use std::{
            fs,
            net::TcpListener,
            thread,
            time::{Duration, Instant},
        };
        use std::{
            process::{Command, Stdio},
            sync::atomic::Ordering,
        };
        #[test]
        #[ignore = "fixed actual child-process entry; only the controlled parent selects it"]
        fn controlled_custodian_process() {
            use std::os::fd::AsFd;
            let setup = gateway_native_import_tools::fixed_fixture::prepare_custodian().unwrap();
            let registered = RegisteredRenewalOwner::configured(
                "native-owner".into(),
                setup.home.to_str().unwrap().into(),
            )
            .unwrap();
            let store =
                ExistingAccountImportStore::configured(&registered, setup.account_id_hash).unwrap();
            let inspector = std::env::current_exe()
                .unwrap()
                .parent()
                .unwrap()
                .parent()
                .unwrap()
                .join("uos-native-owner-inspect");
            assert!(
                inspector.is_file(),
                "build exact fixture artifact before controlled parent"
            );
            let inspector = fs::canonicalize(inspector).unwrap();
            let inspector_hash = sha256_hex(&fs::read(&inspector).unwrap());
            let uid = rustix::process::getuid().as_raw();
            let owner = store
                .prepare_exclusive_owner(
                    &registered,
                    uid,
                    &[uid + 1],
                    &inspector,
                    &inspector_hash,
                    Instant::now() + Duration::from_secs(15),
                )
                .unwrap();
            let mut runtime = owner.prepare_runtime().unwrap();
            runtime
                .start_owned_writer_fixture(Instant::now() + Duration::from_secs(15))
                .unwrap();
            let limit = Instant::now() + Duration::from_secs(5);
            while !setup.home.join("writer-ready").is_file() {
                assert!(Instant::now() < limit);
                thread::sleep(Duration::from_millis(5));
            }
            let mut runtime = runtime.into_custodian().unwrap();
            let input = std::io::stdin().as_fd().try_clone_to_owned().unwrap();
            let listener = TcpListener::from(input);
            assert!(listener.local_addr().unwrap().ip().is_loopback());
            let (client, _) = listener.accept().unwrap();
            drop(listener);
            let result = runtime.serve_fixture(
                client,
                &serde_json::to_vec(&setup.config).unwrap(),
                setup.plan,
            );
            if let Err(error) = result {
                drop(runtime);
                eprintln!(
                    "CONTROLLED_NATIVE_REFUSAL {}",
                    error
                        .envelope
                        .error
                        .code
                        .as_deref()
                        .unwrap_or("native_import_unavailable")
                );
                std::process::exit(79);
            }
        }

        mod negatives {
            use super::*;
            use std::net::Shutdown;
            #[test]
            fn actual_quiet_client_eof_before_and_after_started_never_writes_or_publishes()
            -> Result<(), GatewayError> {
                for phase in ["start", "started_handler"] {
                    let root = CaseRoot::new();
                    let store = base::Store::default();
                    let account = setup(&store, &root.path().join("home"))?;
                    let servers = Servers::start(store.clone(), root.path().join("home"));
                    let pause = PauseHandle::install(&servers, phase, 1);
                    let (mut child, address) = start_custodian(&root, &servers, account.clone());
                    let request = original_request(&store, &root, account, address)?;
                    pause.wait();
                    request
                        .socket
                        .shutdown(Shutdown::Both)
                        .map_err(|_| base::fail())?;
                    pause.resume();
                    let response = request.reply.join().map_err(|_| base::fail())??;
                    wait_failure(&mut child);
                    no_native_effect(&root, &servers, &response);
                }
                Ok(())
            }
            #[test]
            fn actual_custodian_death_after_stop_closes_live_actor_without_late_effect()
            -> Result<(), GatewayError> {
                let root = CaseRoot::new();
                let store = base::Store::default();
                let account = setup(&store, &root.path().join("home"))?;
                let servers = Servers::start(store.clone(), root.path().join("home"));
                let pause = PauseHandle::install(&servers, "started_handler", 3);
                let (mut child, address) = start_custodian(&root, &servers, account.clone());
                let request = original_request(&store, &root, account, address)?;
                pause.wait();
                assert_writer_stopped(&root);
                // Kill only the retained actual custodian Child, not an inferred PID/group.
                let mut custodian = child.child.take().ok_or_else(base::fail)?;
                custodian.kill().map_err(|_| base::fail())?;
                assert!(!custodian.wait().map_err(|_| base::fail())?.success());
                pause.resume();
                let response = request.reply.join().map_err(|_| base::fail())??;
                no_native_effect(&root, &servers, &response);
                assert_writer_stopped(&root);
                Ok(())
            }
            #[test]
            fn actual_admin_revocation_before_native_write_refuses_the_same_started_claim()
            -> Result<(), GatewayError> {
                let root = CaseRoot::new();
                let store = base::Store::default();
                let account = setup(&store, &root.path().join("home"))?;
                let servers = Servers::start(store.clone(), root.path().join("home"));
                let pause = PauseHandle::install(&servers, "started_handler", 3);
                let (mut child, address) = start_custodian(&root, &servers, account.clone());
                let request = original_request(&store, &root, account, address)?;
                pause.wait();
                assert_writer_stopped(&root);
                revoke_admin(&store);
                pause.resume();
                let response = request.reply.join().map_err(|_| base::fail())??;
                wait_failure(&mut child);
                no_native_effect(&root, &servers, &response);
                assert_eq!(
                    servers
                        .traffic
                        .phases
                        .lock()
                        .map_err(|_| base::fail())?
                        .iter()
                        .filter(|phase| **phase == "start")
                        .count(),
                    1
                );
                Ok(())
            }
            #[test]
            fn actual_original_deadline_expiry_after_stop_never_renews_mutation_authority()
            -> Result<(), GatewayError> {
                let root = CaseRoot::new();
                let store = base::Store::default();
                let account = setup(&store, &root.path().join("home"))?;
                let servers = Servers::start(store.clone(), root.path().join("home"));
                let pause = PauseHandle::install(&servers, "started_handler", 3);
                let (mut child, address) = start_custodian(&root, &servers, account.clone());
                let request = original_request(&store, &root, account, address)?;
                pause.wait();
                assert_writer_stopped(&root);
                let remaining = request
                    .deadline_ms
                    .checked_sub(now())
                    .map_err(|_| base::fail())?;
                assert!(remaining > 0 && remaining <= 45_000);
                thread::sleep(Duration::from_millis(remaining as u64 + 30));
                pause.resume();
                let response = request.reply.join().map_err(|_| base::fail())??;
                wait_failure(&mut child);
                no_native_effect(&root, &servers, &response);
                Ok(())
            }
            #[test]
            fn actual_revoked_publication_retains_native_receipt_but_never_source_success()
            -> Result<(), GatewayError> {
                let root = CaseRoot::new();
                let store = base::Store::default();
                let account = setup(&store, &root.path().join("home"))?;
                let servers = Servers::start(store.clone(), root.path().join("home"));
                let pause = PauseHandle::install(&servers, "publication", 1);
                let (mut child, address) = start_custodian(&root, &servers, account.clone());
                let request = original_request(&store, &root, account, address)?;
                pause.wait();
                assert_writer_stopped(&root);
                assert_ne!(
                    fs::read(root.path().join("home/auth.json")).map_err(|_| base::fail())?,
                    OLD
                );
                assert!(
                    fs::read_dir(root.path().join("home"))
                        .map_err(|_| base::fail())?
                        .any(|entry| {
                            entry
                                .map_err(|_| base::fail())?
                                .file_name()
                                .to_string_lossy()
                                .ends_with(".receipt")
                        })
                );
                revoke_admin(&store);
                pause.resume();
                let response = request.reply.join().map_err(|_| base::fail())??;
                wait_failure(&mut child);
                assert!(!response.starts_with(b"HTTP/1.1 200 "));
                let record = futures_executor::block_on(store.read(&codex_source_pool_key()))
                    .map_err(|_| base::fail())?
                    .ok_or_else(base::fail)?;
                let pool = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
                    record.json_value().map_err(|_| base::fail())?,
                )
                .map_err(|_| base::fail())?
                .ok_or_else(base::fail)?;
                assert_eq!(
                    pool.accounts[0].access_token.expose(),
                    "SYNTHETIC_OLD_ACCESS"
                );
                assert_ne!(
                    fs::read(root.path().join("home/auth.json")).map_err(|_| base::fail())?,
                    OLD,
                    "uncertain native durability must not be rolled back or erased"
                );
                assert!(
                    !servers
                        .traffic
                        .phases
                        .lock()
                        .map_err(|_| base::fail())?
                        .contains(&"catalog_seed")
                );
                assert_eq!(servers.traffic.oauth.load(Ordering::SeqCst), 0);
                Ok(())
            }
            #[test]
            fn actual_lost_publication_reply_reconciles_only_the_same_native_receipt()
            -> Result<(), GatewayError> {
                let root = CaseRoot::new();
                let store = base::Store::default();
                let account = setup(&store, &root.path().join("home"))?;
                let servers = Servers::start(store.clone(), root.path().join("home"));
                servers
                    .traffic
                    .drop_publication
                    .store(true, Ordering::SeqCst);
                let (mut child, address) = start_custodian(&root, &servers, account.clone());
                let (response, _) =
                    request(&store, &root.path().join("home"), account, address, NEW)?;
                assert!(response.starts_with(b"HTTP/1.1 200 "));
                child.wait_clean();
                let phases = servers.traffic.phases.lock().map_err(|_| base::fail())?;
                assert_eq!(phases.iter().filter(|phase| **phase == "start").count(), 1);
                assert_eq!(
                    phases
                        .iter()
                        .filter(|phase| **phase == "publication")
                        .count(),
                    1,
                    "no HTTP publication retry or new request digest is allowed"
                );
                assert_eq!(
                    fs::read_dir(root.path().join("home"))
                        .map_err(|_| base::fail())?
                        .filter(|entry| entry
                            .as_ref()
                            .map_err(|_| base::fail())?
                            .file_name()
                            .to_string_lossy()
                            .ends_with(".receipt"))
                        .count(),
                    1
                );
                assert_eq!(servers.traffic.models.load(Ordering::SeqCst), 1);
                assert_eq!(servers.traffic.oauth.load(Ordering::SeqCst), 0);
                assert_writer_stopped(&root);
                Ok(())
            }

            #[test]
            fn fixture_enabled_ordinary_entrypoints_refuse_before_reading_stdin()
            -> Result<(), GatewayError> {
                assert!(gateway_native_connector::artifact::require_production_artifact().is_err());
                for name in [
                    "uos-native",
                    "uos-native-supervisor",
                    "uos-native-request",
                    "uos-native-connector",
                    "uos-native-import",
                ] {
                    let binary = std::env::current_exe()
                        .unwrap()
                        .parent()
                        .unwrap()
                        .parent()
                        .unwrap()
                        .join(name);
                    assert!(
                        binary.is_file(),
                        "build exact fixture artifact before controlled parent"
                    );
                    let binary = fs::canonicalize(binary).unwrap();
                    // The write end remains open and empty. Any stdin read would block.
                    let mut child = Command::new(binary)
                        .env_clear()
                        .stdin(Stdio::piped())
                        .stdout(Stdio::piped())
                        .stderr(Stdio::piped())
                        .spawn()
                        .map_err(|_| base::fail())?;
                    let input = child.stdin.take().ok_or_else(base::fail)?;
                    let deadline = Instant::now() + Duration::from_secs(2);
                    loop {
                        if let Some(status) = child.try_wait().map_err(|_| base::fail())? {
                            assert_eq!(
                                status.code(),
                                Some(if name == "uos-native" { 1 } else { 78 }),
                                "ordinary entrypoint {name}"
                            );
                            break;
                        }
                        if Instant::now() >= deadline {
                            child.kill().map_err(|_| base::fail())?;
                            child.wait().map_err(|_| base::fail())?;
                            panic!("fixture-enabled {name} did not refuse before stdin");
                        }
                        thread::sleep(Duration::from_millis(5));
                    }
                    drop(input);
                    let output = child.wait_with_output().map_err(|_| base::fail())?;
                    assert!(
                        output.stdout.is_empty(),
                        "no listener or service startup may occur"
                    );
                    if name == "uos-native" {
                        assert!(
                            String::from_utf8_lossy(&output.stderr)
                                .contains("native_fixture_artifact_forbidden")
                        );
                    }
                }
                Ok(())
            }
        }
    }
    mod foundation_tests {
        use gateway_native_import_flow_process::fixture::assert_owned_canonical_directory;
        use std::{fs, path::PathBuf, process::Command, time::Duration};
        #[test]
        #[ignore = "Only the fixed owned-process fixture launcher executes this child"]
        fn controlled_async_writer_child() {
            assert!(
                Command::new("/bin/true").spawn().is_err(),
                "mandatory fixed profile must deny unlisted exec"
            );
            assert!(
                Command::new(std::env::current_exe().unwrap())
                    .arg("--list")
                    .spawn()
                    .is_err(),
                "mandatory process-fork denial must also block the otherwise allowed executable"
            );
            let home = PathBuf::from(std::env::var_os("UOS_NATIVE_OWNER_TEST_DIRECTORY").unwrap());
            // spawn_fixture clears ambient TMPDIR. The sealed fixture-only home came
            // from the parent's canonical owned temp root, not a dated path spelling.
            assert_owned_canonical_directory(home.parent().unwrap());
            assert_owned_canonical_directory(&home);
            fs::write(home.join("writer-ready"), b"process-denial-observed").unwrap();
            // Bounded fixture lifetime (50s at5ms); the parent must actually stop/reap it well before this bound.
            for count in 0u64..10_000 {
                fs::write(
                    home.join("async-native-write"),
                    format!("synthetic-write-{count}"),
                )
                .unwrap();
                std::thread::sleep(Duration::from_millis(5));
            }
        }
    }
}
