#![cfg(all(target_os = "macos", feature = "native-import-fixture"))]
mod existing_account_import {
    mod foundation_tests {
        use gateway_native_import_flow_process::fixture::{
            Home, assert_owned_canonical_directory, deadline, owner, wait_ready,
        };
        use std::{
            fs,
            path::PathBuf,
            process::Command,
            time::{Duration, Instant},
        };
        #[test]
        fn actual_owned_async_child_stop_wait_and_dirty_epoch_do_not_infer_external_absence() {
            let home = Home::new();
            let custody = owner(&home);
            let mut runtime = custody.prepare_runtime().unwrap();
            assert!(!runtime.has_settled_owned_writer());
            // The first-party test child obtains only a synthetic output path through argv after '--'.
            runtime.start_owned_writer_fixture(deadline()).unwrap();
            // No fake successful child handshake is accepted. The child test entry below performs the
            // actual fork/exec denial check; this test validates actual Child ownership and settlement.
            wait_ready(&home);
            assert!(!runtime.has_settled_owned_writer());
            assert!(
                fs::read(home.0.join("async-native-write"))
                    .unwrap()
                    .starts_with(b"synthetic-write-")
            );
            // Drop is deliberately UNKNOWN even when best-effort cleanup reaps our actual child.
            drop(runtime);
            let replacement = owner(&home);
            assert!(replacement.prepare_runtime().is_err());
        }
        #[test]
        fn actual_owned_child_reap_records_stopped_but_reopened_marker_grants_no_lease() {
            let home = Home::new();
            let custody = owner(&home);
            let mut runtime = custody.prepare_runtime().unwrap();
            runtime.start_owned_writer_fixture(deadline()).unwrap();
            wait_ready(&home);
            runtime.stop_owned_writer_fixture(deadline()).unwrap();
            assert!(runtime.has_settled_owned_writer());
            let final_bytes = fs::read(home.0.join("async-native-write")).unwrap();
            std::thread::sleep(Duration::from_millis(25));
            assert_eq!(
                fs::read(home.0.join("async-native-write")).unwrap(),
                final_bytes
            );
            drop(runtime);
            let replacement = owner(&home).prepare_runtime().unwrap();
            assert!(!replacement.has_settled_owned_writer());
        }

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
