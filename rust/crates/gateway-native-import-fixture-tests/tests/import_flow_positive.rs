#![cfg(all(target_os = "macos", feature = "native-import-fixture"))]
mod existing_account_import {
    mod flow_tests {
        use gateway_auth_credentials::provider_credentials::codex_source_pool_key;
        use gateway_auth_primitives::crypto::sha256_hex;
        use gateway_authority_native_records::owner::RegisteredRenewalOwner;
        use gateway_core::state::AuthorityStore;
        use gateway_native_import_flow_authority::fixture::{MODELS, NEW, Servers, key, setup};
        use gateway_native_import_flow_process::fixture::{
            CaseRoot, assert_fixture_child_binding, request, start_custodian,
        };
        use gateway_native_import_store::store::ExistingAccountImportStore;
        use gateway_native_import_test_support::fixture as base;
        use gateway_source::{error::GatewayError, source_json::SourceJson};
        use std::sync::atomic::Ordering;
        use std::{
            fs,
            net::TcpListener,
            thread,
            time::{Duration, Instant},
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

        #[test]
        fn fixture_child_paths_refuse_escaping_and_symlinked_bindings() {
            let root = CaseRoot::new();
            let base = root.base().to_path_buf();
            let home = root.path().join("home");
            assert_fixture_child_binding(&base, root.path(), &home);
            assert!(
                std::panic::catch_unwind(|| {
                    assert_fixture_child_binding(&base, &home, &home.join("home"));
                })
                .is_err()
            );
            assert!(
                std::panic::catch_unwind(|| {
                    assert_fixture_child_binding(&base, root.path(), &base);
                })
                .is_err()
            );
            let alias = root.path().join("root-alias");
            std::os::unix::fs::symlink(root.path(), &alias).unwrap();
            assert!(
                std::panic::catch_unwind(|| {
                    assert_fixture_child_binding(&base, &alias, &home);
                })
                .is_err()
            );
        }

        #[test]
        fn actual_existing_account_original_start_owned_child_custody_durability_and_publication()
        -> Result<(), GatewayError> {
            let root = CaseRoot::new();
            let store = base::Store::default();
            let account = setup(&store, &root.path().join("home"))?;
            let servers = Servers::start(store.clone(), root.path().join("home"));
            let (mut child, address) = start_custodian(&root, &servers, account.clone());
            let (response, _metadata) =
                request(&store, &root.path().join("home"), account, address, NEW)?;
            assert!(
                response.starts_with(b"HTTP/1.1 200 "),
                "real source success required, not a dummy200 or narrowed unavailable path: {}",
                String::from_utf8_lossy(&response)
            );
            child.wait_clean();
            let header_end = response
                .windows(4)
                .position(|part| part == b"\r\n\r\n")
                .ok_or_else(base::fail)?
                + 4;
            let headers = String::from_utf8_lossy(&response[..header_end]).to_ascii_lowercase();
            assert!(headers.contains("x-uos-upstream: chatgpt_codex\r\n"));
            let result: serde_json::Value =
                serde_json::from_slice(&response[header_end..]).map_err(|_| base::fail())?;
            assert_eq!(
                result.as_object().ok_or_else(base::fail)?.len(),
                12,
                "source completion fields may not be silently omitted"
            );
            assert_eq!(result["stored"], true);
            assert_eq!(result["refreshed"], false);
            assert_eq!(result["account_id"], "synthetic-account");
            assert_eq!(result["account_count"], 1);
            assert_eq!(
                result["account_ids"],
                serde_json::json!(["synthetic-account"])
            );
            assert!(result["access_token_expires_at_ms"].is_null());
            assert_eq!(result["upstream_status"], 200);
            assert_eq!(
                result["upstream_content_type"],
                "application/json; charset=utf-8"
            );
            assert_eq!(result["normalized_snapshot_updated"], true);
            assert_eq!(
                result["catalog_seeded"], true,
                "normal source success must actually seed its versioned cache"
            );
            assert_eq!(result["models"]["count"], 1);
            assert_eq!(result["models"]["source"], "chatgpt_codex");
            assert_eq!(result["models"]["client_version"], "0.163.0");
            let pool_record = futures_executor::block_on(store.read(&codex_source_pool_key()))
                .map_err(|_| base::fail())?
                .ok_or_else(base::fail)?;
            let pool = gateway_auth_credentials::provider_credentials::parse_codex_source_pool(
                pool_record.json_value().map_err(|_| base::fail())?,
            )
            .map_err(|_| base::fail())?
            .ok_or_else(base::fail)?;
            assert_eq!(
                result["updated_at_ms"].as_f64(),
                Some(pool.accounts[0].updated_at_ms)
            );
            assert_eq!(
                pool.accounts[0].access_token.expose(),
                "SYNTHETIC_NEW_ACCESS"
            );
            assert!(pool.accounts[0].refresh_token.expose().is_empty());
            let generation = futures_executor::block_on(
                store.read(&key(&["ubq_ai", "codex_catalog_auth_generation"])),
            )
            .map_err(|_| base::fail())?
            .ok_or_else(base::fail)?;
            let cache = futures_executor::block_on(store.read(&key(&[
                "ubq_ai",
                "codex_catalog",
                "0.163.0",
            ])))
            .map_err(|_| base::fail())?
            .ok_or_else(base::fail)?;
            let cache = cache.json_value().map_err(|_| base::fail())?;
            assert_eq!(cache["auth_generation"], generation.value);
            assert_eq!(cache["source_sha256"], sha256_hex(MODELS));
            assert!(cache["chunk_count"].as_u64().ok_or_else(base::fail)? > 0);
            assert!(
                cache["source_chunk_count"]
                    .as_u64()
                    .ok_or_else(base::fail)?
                    > 0
            );
            assert_eq!(cache["content_type"], "application/json; charset=utf-8");
            let bytes = fs::read(root.path().join("home/auth.json")).map_err(|_| base::fail())?;
            let document = SourceJson::parse_bytes(&bytes).map_err(|_| base::fail())?;
            let next = gateway_native_import_source::documents::credentials(&document)
                .map_err(|_| base::fail())?;
            assert_eq!(next.access_token.expose(), "SYNTHETIC_NEW_ACCESS");
            assert_eq!(next.refresh_token.expose(), "SYNTHETIC_NEW_REFRESH");
            assert_eq!(
                gateway_native_import_source::documents::member(&document, "OPENAI_API_KEY")
                    .map_err(|_| base::fail())?
                    .ok_or_else(base::fail)?
                    .as_str(),
                r#""SYNTHETIC_INDEPENDENT""#
            );
            assert_eq!(
                gateway_native_import_source::documents::member(&document, "unrelated")
                    .map_err(|_| base::fail())?
                    .ok_or_else(base::fail)?
                    .as_str(),
                r#""\ud800""#
            );
            let witness =
                fs::read(root.path().join("home/async-native-write")).map_err(|_| base::fail())?;
            thread::sleep(Duration::from_millis(25));
            assert_eq!(
                fs::read(root.path().join("home/async-native-write")).map_err(|_| base::fail())?,
                witness,
                "actual writer must have stopped/reaped before credential mutation"
            );
            assert_eq!(servers.traffic.models.load(Ordering::SeqCst), 1);
            assert_eq!(servers.traffic.oauth.load(Ordering::SeqCst), 0);
            let captured = servers.traffic.authority.lock().map_err(|_| base::fail())?;
            assert!(captured.iter().any(|body| {
                body.windows(b"codex_auth_import_publish".len())
                    .any(|w| w == b"codex_auth_import_publish")
            }));
            for wire in captured.iter() {
                let text = String::from_utf8_lossy(wire);
                assert!(!text.contains("SYNTHETIC_NEW_REFRESH"));
                assert!(!text.contains(&sha256_hex(NEW)));
                assert!(!text.contains(&sha256_hex(&bytes)));
            }
            Ok(())
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
